#include "c_types.h"
#include "mem.h"
#include "user_interface.h"
#include "ets_sys.h"
#include "osapi.h"
#include "espconn.h"
#include "woz_monitor.h"
#include "spi_flash.h"
#include <ip_addr.h>

#define RAM_SIZE 0x5000
#define INSTRUCTIONS_CHUNK 10000

#define TERM_WIDTH 40
#define TERM_HEIGHT 24

#define SPACE 0x20


static volatile os_timer_t emulator_callback_timer, cursor_timer;

uint8_t computer_ram[RAM_SIZE],
        terminal_ram[TERM_WIDTH * TERM_HEIGHT];

uint16_t load_target_start;

uint32_t current_start,
         current_end,
         loop_counter = 0;

/* Current terminal row and column */
uint8_t term_x = 0,
        term_y = 0,
        cursor_visible = 0,
        cursor_disabled = 0;

struct pia6821 {
        uint8_t keyboard_register;
        uint8_t keyboard_control;
        uint8_t display_register;
        uint8_t display_control;

}   pia = {0};


/* ---------- Function definitions ------------ */

void ICACHE_FLASH_ATTR reset_emulator() {
        term_x = 0;
        term_y = 0;

        ets_memset( computer_ram, 0xff, sizeof(computer_ram) );
        ets_memset( terminal_ram, 0b100000, sizeof(terminal_ram) );
        reset6502();
}

uint8_t read6502(uint16_t address) {
        /* Address in RAM */
        if (address < RAM_SIZE)
                return computer_ram[address];

        /* 4kB of RAM (0x4000-0x5000) is logically mapped to memory bank 0xE000, needed for BASIC. */
        else if ((address & 0xF000) == 0xE000)
                return computer_ram[address - 0xA000];

        /* PIA peripheral interface */
        else if ((address & 0xFFF0) == 0xD010) {
                /* Set keyboard control register to 0 if key was read */
                if (address == 0xD010) {
                        pia.keyboard_control = 0x00;
                }

                return *(&pia.keyboard_register + address - 0xD010);
        }

        /* Address belongs to Woz Monitor ROM (0xFF00 - 0xFFFF) */
        else if ((address & 0xFF00) == 0xFF00)
                return woz_monitor[address - 0xFF00];

        /* Default value */
        return 0xff;
}


void ICACHE_FLASH_ATTR toggle_cursor() {
        uint8_t i;

        cursor_visible ^= 1;
        terminal_ram[term_y * TERM_WIDTH + term_x] = cursor_visible | cursor_disabled ? 0x20 : 0x00;
}


void ICACHE_FLASH_ATTR terminal_write(uint8_t value) {
        // decimal 32 to 94

        /* When changing the terminal_ram, disable cursor first */
        cursor_disabled = 1;

        /* Commit change */
        toggle_cursor();

        /* End of line reached or return pressed */
        if(term_x > 39 || value == 0x0D || value == 0x0A) {
                term_x = 0;

                if(term_y >= 23) {
                        /* Scroll 1 line up (copy 23 text lines only, blank the last one) */
                        ets_memcpy(terminal_ram, &terminal_ram[TERM_WIDTH], TERM_WIDTH * (TERM_HEIGHT - 1));
                        ets_memset(terminal_ram + TERM_WIDTH * (TERM_HEIGHT - 1), SPACE, TERM_WIDTH);

                }
                else
                        term_y++;

        }

        /* Only printable characters go to terminal RAM. Other characters don't move the cursor either. */
        // 32 to 126 inclusive = 94 displayable characters
        if (value >= 0x20 && value <= 0x7E) {
                terminal_ram[term_y * TERM_WIDTH + term_x] = value & 0x3F;
                term_x++;
        }

        /* Enable cursor again */
        cursor_disabled = 0;
}


void write6502(uint16_t address, uint8_t value)
{
        if(address < RAM_SIZE) {
                computer_ram[address] = value;
        }

        /* Address belongs to a 4kB bank mapped at (0xE000 - 0xF000), translate it to real RAM 0x4000-0x5000
         * this is needed to run Apple BASIC */
        else if((address & 0xF000) == 0xE000) {
                computer_ram[address - 0xA000] = value;
        }

        /* Write to PIA chip. */
        else if (address == 0xD010) {
                pia.keyboard_register = value;

                /* If a key was pressed, write to keyboard control register as well */
                pia.keyboard_control = 0xFF;
        }
        else if (address == 0xD012) {
                terminal_write(value ^ 0x80);
        }
}


static void ICACHE_FLASH_ATTR emulator_task(os_event_t *events)
{
        current_start = system_get_time();
        exec6502(INSTRUCTIONS_CHUNK);
        current_end = system_get_time();
}


static void ICACHE_FLASH_ATTR dataRecvCallback(void *arg, char *pusrdata, unsigned short lenght){

        char input_character = *pusrdata;

        /* Convert lowercase to uppercase */
        if (input_character > 0x60 && input_character < 0x7B)
                input_character ^= 0x20;

        /* Convert LF to CR */
        else if (input_character == 0x0A)
                input_character = 0x0D;

        /* Convert backspace to "rub out" */
        else if (input_character == 0x7F)
                input_character = '_';

        /* Enable CPU reset from telnet (Ctrl + C) */
        else if (input_character == 0x03) {
                reset_emulator();
                return;
        }

        write6502(0xd010, input_character | 0x80);
}


static void ICACHE_FLASH_ATTR connectionCallback(void *arg){
        struct espconn *telnet_server = arg;

	/* Welcome message and force character mode on client */
        char *welcome_message = "Welcome to Espple!\n\xff\xfd\x22\xff\xfb\x01";

        espconn_regist_recvcb(telnet_server, dataRecvCallback);
        espconn_send(telnet_server, welcome_message, strlen(welcome_message));
}


void tftp_server_recv(void *arg, char *pdata, unsigned short len)
{
        struct espconn* udp_server_local = arg;
        uint8_t ack[] = {0x00, 0x04, 0x00, 0x00};

        if (len < 4)
                return;

        /* Write request, this is the first package */
        if (pdata[1] == 0x02) {
                load_target_start = (computer_ram[0x27] << 8) + computer_ram[0x26];
                if (load_target_start >= 0xE000)
                        load_target_start -= 0xA000;
        }

        /* Data packet */
        else if(pdata[1] == 0x03) {
                /* Copy sequence number into ACK packet and send it */
                ets_memcpy(&ack[2], &pdata[2], 2);

                ets_memcpy(&computer_ram[load_target_start], pdata + 4, len - 4);
                load_target_start += (len - 4);

        }

        espconn_send(udp_server_local, ack, 4);
}

void ICACHE_FLASH_ATTR sendString(unsigned char *string){
    //For each char in string, write to the terminal
    for (int i = 0; i < strlen(string); i++){
            terminal_write(string[i]);
    }
}

void ICACHE_FLASH_ATTR printAllCharactersInFont(){
    // ascii values < 32 are control codes
    // the terminal function won't print anything over decimal 126
    /*
    numbers match ASCII:
    48: 0
    49: 1
    50: 2
    51: 3
    52: 4
    53: 5
    54: 6
    55: 7
    56: 8
    57: 9
    */
    terminal_newLine();
    sendString("CHARACTER SIZE: 6W PIXELS X 8H PIXELS");
    terminal_newLine();
    sendString("CHARACTER SET:");
    terminal_newLine();
    // display them again this time in compact form
    for(int i = 32; i < 96; i++){
        terminal_write(i);
        if (i % 32 == 0) {
            terminal_newLine();
        }
    }
    terminal_newLine();
    terminal_newLine();
    // current character set seems to start repeating at 96
    for(int i = 32; i < 96; i++){
        if (i == 32) {sendString(" 32: ");}
        if (i == 33) {sendString(" 33: ");}
        if (i == 34) {sendString(" 34: ");}
        if (i == 35) {sendString(" 35: ");}
        if (i == 36) {sendString(" 36: ");}
        if (i == 37) {sendString(" 37: ");}
        if (i == 38) {sendString(" 38: ");}
        if (i == 39) {sendString(" 39: ");}
        if (i == 40) {sendString(" 40: ");}
        if (i == 41) {sendString(" 41: ");}
        if (i == 42) {sendString(" 42: ");}
        if (i == 43) {sendString(" 43: ");}
        if (i == 44) {sendString(" 44: ");}
        if (i == 45) {sendString(" 45: ");}
        if (i == 46) {sendString(" 46: ");}
        if (i == 47) {sendString(" 47: ");}
        if (i == 48) {sendString(" 48: ");}
        if (i == 49) {sendString(" 49: ");}
        if (i == 50) {sendString(" 50: ");}
        if (i == 51) {sendString(" 51: ");}
        if (i == 52) {sendString(" 52: ");}
        if (i == 53) {sendString(" 53: ");}
        if (i == 54) {sendString(" 54: ");}
        if (i == 55) {sendString(" 55: ");}
        if (i == 56) {sendString(" 56: ");}
        if (i == 57) {sendString(" 57: ");}
        if (i == 58) {sendString(" 58: ");}
        if (i == 59) {sendString(" 59: ");}
        if (i == 60) {sendString(" 60: ");}
        if (i == 61) {sendString(" 61: ");}
        if (i == 62) {sendString(" 62: ");}
        if (i == 63) {sendString(" 63: ");}
        if (i == 64) {sendString(" 64: ");}
        if (i == 65) {sendString(" 65: ");}
        if (i == 66) {sendString(" 66: ");}
        if (i == 67) {sendString(" 67: ");}
        if (i == 68) {sendString(" 68: ");}
        if (i == 69) {sendString(" 69: ");}
        if (i == 70) {sendString(" 70: ");}
        if (i == 71) {sendString(" 71: ");}
        if (i == 72) {sendString(" 72: ");}
        if (i == 73) {sendString(" 73: ");}
        if (i == 74) {sendString(" 74: ");}
        if (i == 75) {sendString(" 75: ");}
        if (i == 76) {sendString(" 76: ");}
        if (i == 77) {sendString(" 77: ");}
        if (i == 78) {sendString(" 78: ");}
        if (i == 79) {sendString(" 79: ");}
        if (i == 80) {sendString(" 80: ");}
        if (i == 81) {sendString(" 81: ");}
        if (i == 82) {sendString(" 82: ");}
        if (i == 83) {sendString(" 83: ");}
        if (i == 84) {sendString(" 84: ");}
        if (i == 85) {sendString(" 85: ");}
        if (i == 86) {sendString(" 86: ");}
        if (i == 87) {sendString(" 87: ");}
        if (i == 88) {sendString(" 88: ");}
        if (i == 89) {sendString(" 89: ");}
        if (i == 90) {sendString(" 90: ");}
        if (i == 91) {sendString(" 91: ");}
        if (i == 92) {sendString(" 92: ");}
        if (i == 93) {sendString(" 93: ");}
        if (i == 94) {sendString(" 94: ");}
        if (i == 95) {sendString(" 95: ");}

        terminal_write(i);
        if (i % 6 == 0) {
            terminal_newLine();
        }
    }

    terminal_newLine();
}

void ICACHE_FLASH_ATTR setCursorPosition(uint8_t x, uint8_t y) {
    // prevent cursor going beyond edge of screen
    if(x >= TERM_WIDTH) {
        x = TERM_WIDTH - 1;
    }
    if(y >= TERM_HEIGHT) {
        x = TERM_HEIGHT - 1;
    }
    term_x = x;
    term_y = y;
}

void ICACHE_FLASH_ATTR terminal_space() {
    terminal_write(32);
}

void ICACHE_FLASH_ATTR terminal_newLine() {
    terminal_write(10);
}

void ICACHE_FLASH_ATTR terminal_clearScreen() {
    for(int i = 0; i < TERM_WIDTH * TERM_HEIGHT; i++){
        terminal_write(32); // space
    }
    term_x = 0;
    term_y = 0;
}

void ICACHE_FLASH_ATTR terminal_fillScreen() {
    //for(int i = 0; i < TERM_WIDTH * TERM_HEIGHT; i++){
    for(int i = 0; i < 4; i++){
        sendString("0123456789");
    }
}

void ICACHE_FLASH_ATTR startup() {
        terminal_clearScreen();
        //char *str1 = "0123456789";
        //sendString(str1);
        terminal_newLine();
        sendString("ESP8266 STARTED");
        terminal_newLine();
        terminal_newLine();
        sendString("40X24 TEXT, 960?X240 PIXEL RESOLUTION");
        terminal_newLine();
        for(int i = 0; i < 4; i++){
            sendString("0123456789");
        }
        terminal_newLine();
        printAllCharactersInFont();
        terminal_newLine();
        //setCursorPosition(0,0);
}

void ICACHE_FLASH_ATTR user_init(void)
{
        uint16_t ui_address;
        struct ip_info ip_address;

        char ssid[32] = "SSID";
        char password[32] = "PASSWORD";

        uart_div_modify(0, UART_CLK_FREQ / 115200);

        uint32 credentials[16] = {0};

        spi_flash_read(0x3c000, (uint32 *)&credentials[0], 16 * sizeof(uint32));

        struct station_config stationConf;

        ets_strcpy(&stationConf.ssid, &credentials[0]);
        ets_strcpy(&stationConf.password, &credentials[8]);

        current_start = system_get_time();

        //reset_emulator();
        testi2s_init();

        system_update_cpu_freq( SYS_CPU_160MHZ );

        /* Create a 10ms timer to call back the emulator task function periodically */
        //os_timer_setfn(&emulator_callback_timer, (os_timer_func_t *) emulator_task, NULL);
        //os_timer_arm(&emulator_callback_timer, 10, 1);

        /* Toggle cursor every 1000 ms */
        os_timer_setfn(&cursor_timer, (os_timer_func_t *) toggle_cursor, NULL);
        os_timer_arm(&cursor_timer, 600, 1);

        startup();

        /* Initialize wifi connection */
        wifi_set_opmode( STATION_MODE );

        wifi_station_set_config(&stationConf);
        wifi_set_phy_mode(PHY_MODE_11B);
        wifi_station_set_auto_connect(1);
        wifi_station_connect();


        /* TFTP server */
        struct espconn *tftp_server = (struct espconn *)os_zalloc(sizeof(struct espconn));
        ets_memset( tftp_server, 0, sizeof( struct espconn ) );
        tftp_server->type = ESPCONN_UDP;
        tftp_server->proto.udp = (esp_udp *)os_zalloc(sizeof(esp_udp));
        tftp_server->proto.udp->local_port = 69;

        espconn_regist_recvcb(tftp_server, tftp_server_recv);
        espconn_create(tftp_server);


        /* Telnet server */
        struct espconn *telnet_server = (struct espconn *)os_zalloc(sizeof(struct espconn));
        ets_memset(telnet_server, 0, sizeof(struct espconn));

        espconn_create(telnet_server);
        telnet_server->type = ESPCONN_TCP;
        telnet_server->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
        telnet_server->proto.tcp->local_port = 23;
        espconn_regist_connectcb(telnet_server, connectionCallback);

        espconn_accept(telnet_server);
    	espconn_regist_time(telnet_server, 3600, 0);
}
