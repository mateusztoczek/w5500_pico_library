
#include "w5500_port.h"
 
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "hardware/flash.h"
#include "wizchip_conf.h"
#include "socket.h"
#include "DHCP/dhcp.h"
#include "DNS/dns.h"
#include "pico/unique_id.h"
#include "heartbeat.h"


#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define DISCOVERY_LOCAL_PORT 50000
#define DISCOVERY_SERVER_PORT 40001

#define SOCK_DHCP 0
#define SOCK_HTTP 1
#define SOCK_DISCOVERY 2

#define W5500_DHCP_DEFAULT_TIMEOUT_MS 10000u

#define W5500_CFG_FLAG_SERVER_CONFIGURED (1u << 0)
#define W5500_CFG_FLAG_PROVISIONED (1u << 1)
#define W5500_CFG_FLAG_TOKEN_VALID (1u << 2)

#define W5500_MIN_INTERVAL_S 1
#define W5500_MAX_INTERVAL_S 500

#define W5500_LINK_TIMEOUT_MS 5000
#define W5500_LINK_POLL_MS 50

#define DISCOVERY_MAX_ATTEMPTS 5
#define DISCOVERY_RESPONSE_TIMEOUT_MS 2000
#define DISCOVERY_POLL_MS 10

#define W5500_HTTP_TIMEOUT_MS 2000

#define W5500_DEFAULT_SPI_PORT spi0
#define W5500_DEFAULT_PIN_MISO 16
#define W5500_DEFAULT_PIN_CS   17
#define W5500_DEFAULT_PIN_SCK  18
#define W5500_DEFAULT_PIN_MOSI 19
#define W5500_DEFAULT_PIN_RST  20
#define W5500_DEFAULT_PIN_INT  21

static const W5500_Board_Config_t g_default_board_config = {
    .spi_port = W5500_DEFAULT_SPI_PORT,
    .miso_pin = W5500_DEFAULT_PIN_MISO,
    .mosi_pin = W5500_DEFAULT_PIN_MOSI,
    .sck_pin = W5500_DEFAULT_PIN_SCK,
    .cs_pin = W5500_DEFAULT_PIN_CS,
    .rst_pin = W5500_DEFAULT_PIN_RST,
    .int_pin = W5500_DEFAULT_PIN_INT
};


static W5500_Board_Config_t g_board;
static W5500_Network_Config_t g_conn;
static wiz_NetInfo g_netinfo;

static bool g_board_initialized = false;
static bool g_conn_initialized = false;
static bool g_dhcp_initialized = false;

static volatile bool g_dhcp_ip_found = false;
static absolute_time_t g_dhcp_last_tick;
static uint8_t g_dhcp_buffer[1024];
static uint8_t g_udp_discover_buffer[1024];
static uint16_t g_http_local_port = 50000;




static void w5500_cs_select(void){
    gpio_put(g_board.cs_pin, 0);
}

static void w5500_cs_deselect(void){
    gpio_put(g_board.cs_pin, 1);
}

static uint8_t w5500_spi_read_byte(void){
    uint8_t rx = 0;
    spi_read_blocking(g_board.spi_port, 0x00, &rx, 1);
    return rx;
}

static void w5500_spi_write_byte(uint8_t data){
    spi_write_blocking(g_board.spi_port, &data, 1);
}

static void w5500_spi_read_burst(uint8_t *buf, uint16_t len){
    if (buf == NULL || len == 0) return;
    spi_read_blocking(g_board.spi_port, 0x00, buf, len);
}

static void w5500_spi_write_burst(uint8_t *buf, uint16_t len){
    if (buf == NULL || len == 0) return;
    spi_write_blocking(g_board.spi_port, buf, len);
}


void w5500_reset(void){
    gpio_put(g_board.rst_pin, 0);
    sleep_ms(5);
    gpio_put(g_board.rst_pin, 1);
    sleep_ms(50);
}


const W5500_Board_Config_t *W5500_Board_DefaultConfig(void) {
    return &g_default_board_config;
}


int W5500_Board_Init(const W5500_Board_Config_t *cfg) {
    g_board_initialized = false;
    if (cfg == NULL) cfg = W5500_Board_DefaultConfig();

    if (cfg->spi_port == NULL) return -1;
    if (cfg->miso_pin > 29 || cfg->mosi_pin > 29 ||
        cfg->sck_pin > 29 || cfg->cs_pin > 29 ||
        cfg->rst_pin > 29 || cfg->int_pin > 29) {
            return -2;
    }

    g_board = *cfg;
    spi_init(g_board.spi_port, 10 * 1000 * 1000);
    gpio_set_function(g_board.miso_pin, GPIO_FUNC_SPI);
    gpio_set_function(g_board.mosi_pin, GPIO_FUNC_SPI);
    gpio_set_function(g_board.sck_pin,  GPIO_FUNC_SPI);

    gpio_init(g_board.cs_pin);
    gpio_set_dir(g_board.cs_pin, GPIO_OUT);
    gpio_put(g_board.cs_pin, 1);

    gpio_init(g_board.rst_pin);
    gpio_set_dir(g_board.rst_pin, GPIO_OUT);
    gpio_put(g_board.rst_pin, 1);

    gpio_init(g_board.int_pin);
    gpio_set_dir(g_board.int_pin, GPIO_IN);
    gpio_pull_up(g_board.int_pin);

    w5500_reset();

    reg_wizchip_cs_cbfunc(w5500_cs_select, w5500_cs_deselect);
    reg_wizchip_spi_cbfunc(w5500_spi_read_byte, w5500_spi_write_byte);
    reg_wizchip_spiburst_cbfunc(w5500_spi_read_burst, w5500_spi_write_burst);

    uint8_t memsize[2][8] = {{2,2,2,2,2,2,2,2},{2,2,2,2,2,2,2,2}};
    if (ctlwizchip(CW_INIT_WIZCHIP, (void*)memsize) == -1) return -3;
    if (getVERSIONR() != 0x04) return -4;
    g_board_initialized = true;
    return 0;
}


static void W5500_New_Local_MAC(uint8_t mac[6]){
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);

    mac[0] = 0x02;
    mac[1] = 0x46;
    mac[2] = 0x52;
    mac[3] = id.id[5];
    mac[4] = id.id[6];
    mac[5] = id.id[7];
}


void W5500_Default_config(W5500_Network_Config_t *cfg){
    memset(cfg, 0, sizeof(*cfg));
    cfg->magic = W5500_CONFIG_MAGIC;
    W5500_New_Local_MAC(cfg->mac);
    cfg->use_dhcp = true;
    cfg->server_ip[0] = 0;
    cfg->server_port = 0;
    cfg->http_path[0] = '\0';
    cfg->interval_s = 60;
    cfg->config_flags = 0;
}


static bool w5500_is_valid_mac(const uint8_t mac[6]){
    bool all_zero = true;
    bool all_ff = true;

    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) all_zero = false;
        if (mac[i] != 0xFF) all_ff = false;
    }

    if (all_zero || all_ff) return false;
    if (mac[0] & 0x01) return false;

    return true;
}


static bool w5500_is_valid_ipv4_addr(const uint8_t ip[4]){
    bool all_zero = true;
    bool all_ff = true;

    for (int i = 0; i < 4; i++) {
        if (ip[i] != 0x00) all_zero = false;
        if (ip[i] != 0xFF) all_ff = false;
    }

    if (all_zero || all_ff) return false;
    if (ip[0] == 127) return false;
    if (ip[0] >= 224 && ip[0] <= 239) return false;

    return true;
}


static bool w5500_is_valid_netmask(const uint8_t sn[4]){
    if (sn == NULL) return false;

    uint32_t mask = ((uint32_t)sn[0] << 24) | ((uint32_t)sn[1] << 16) | ((uint32_t)sn[2] << 8) | ((uint32_t)sn[3]);
    if (mask == 0u || mask == 0xFFFFFFFFu) return false;
    uint32_t inv = ~mask;

    return (inv & (inv + 1u)) == 0u;
}


static uint32_t w5500_crc32_compute(const void *data, size_t len){
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
            else crc >>= 1;
        }
    }

    return ~crc;
}

static bool w5500_is_valid_crc(const W5500_Network_Config_t *cfg){
    if (cfg == NULL) return false;
    if (cfg->crc == 0u || cfg->crc == 0xFFFFFFFFu) return false;
    uint32_t calculated_crc = w5500_crc32_compute(cfg, offsetof(W5500_Network_Config_t, crc));

    return calculated_crc == cfg->crc;
}


static int W5500_Load_Flash_Config(W5500_Network_Config_t *cfg){
    if (cfg == NULL) return -1;

    const uint8_t *flash_ptr = (const uint8_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    memcpy(cfg, flash_ptr, sizeof(W5500_Network_Config_t));

    return 0;
}


int W5500_Network_Init(const W5500_Network_Config_t *cfg) {
    g_conn_initialized = false;

    if (cfg == NULL) return -1;
    if (!g_board_initialized) return -2;
    if (!w5500_is_valid_mac(cfg->mac)) return -3;
    if (!cfg->use_dhcp) {
        if (!w5500_is_valid_ipv4_addr(cfg->ip)) return -4;
        if (!w5500_is_valid_netmask(cfg->sn)) return -5;
    }

    g_conn = *cfg;

    memset(&g_netinfo, 0, sizeof(g_netinfo));
    memcpy(g_netinfo.mac, cfg->mac, 6);
    g_netinfo.dhcp = cfg->use_dhcp ? NETINFO_DHCP : NETINFO_STATIC;

    if (!cfg->use_dhcp) {
        memcpy(g_netinfo.ip, cfg->ip, 4);
        memcpy(g_netinfo.sn, cfg->sn, 4);
        memcpy(g_netinfo.gw, cfg->gw, 4);
        memcpy(g_netinfo.dns, cfg->dns, 4);
    }

    g_conn_initialized = true;
    return 0;
}


static void DHCP_IP_Assigned(void){
    getIPfromDHCP(g_netinfo.ip);
    getGWfromDHCP(g_netinfo.gw);
    getSNfromDHCP(g_netinfo.sn);
    getDNSfromDHCP(g_netinfo.dns);

    g_netinfo.dhcp = NETINFO_DHCP;
    const int8_t ret = ctlnetwork(CN_SET_NETINFO, (void *)&g_netinfo);
    g_dhcp_ip_found = (ret == 0);
}


static void DHCP_IP_Updated(void){
    DHCP_IP_Assigned();
}


static void DHCP_IP_Conflict(void){
    g_dhcp_ip_found = false;
}


static int W5500_DHCP_Init(void){
    if (g_netinfo.dhcp != NETINFO_DHCP) {
        g_dhcp_initialized = false;
        return 0;
    }

    g_dhcp_initialized = false;
    g_dhcp_ip_found = false;

    if (ctlnetwork(CN_SET_NETINFO, &g_netinfo) == -1) return -1;
    DHCP_init(SOCK_DHCP, g_dhcp_buffer);
    reg_dhcp_cbfunc(DHCP_IP_Assigned, DHCP_IP_Updated, DHCP_IP_Conflict);
    g_dhcp_last_tick = get_absolute_time();
    g_dhcp_initialized = true;

    return 0;
}


int W5500_Network_Poll(void) {
    if (!g_board_initialized) return -1;
    if (!g_conn_initialized) return -2;

    uint8_t link = PHY_LINK_OFF;
    if (ctlwizchip(CW_GET_PHYLINK, &link) == -1) return -3;

    if (link != PHY_LINK_ON) {
        g_dhcp_ip_found = false;
        return -4;
    }

    if (!g_conn.use_dhcp) return 0;
    if (!g_dhcp_initialized) return -5;

    absolute_time_t now = get_absolute_time();

    if (absolute_time_diff_us(g_dhcp_last_tick, now) >= 1000000) {
        DHCP_time_handler();
        g_dhcp_last_tick = now;
    }

    uint8_t dhcp_state = DHCP_run();
    switch (dhcp_state) {
        case DHCP_IP_ASSIGN:
        case DHCP_IP_CHANGED:
        case DHCP_IP_LEASED:
            return g_dhcp_ip_found ? 0 : -6;
        case DHCP_RUNNING:
            return g_dhcp_ip_found ? 0 : 1;
        case DHCP_FAILED:
            g_dhcp_ip_found = false;
            return -7;
        case DHCP_STOPPED:
            g_dhcp_ip_found = false;
            return -8;
        default:
            return -9;
    }
}


static int W5500_DHCP_Connect(uint32_t timeout_ms){
    if (W5500_DHCP_Init() != 0) return -1;

    const absolute_time_t deadline= make_timeout_time_ms(timeout_ms);
    while (!time_reached(deadline)) {
        const absolute_time_t now = get_absolute_time();

        if (absolute_time_diff_us(g_dhcp_last_tick, now) >= 1000000) {
            DHCP_time_handler();
            g_dhcp_last_tick = now;
        }
        const uint8_t dhcp_state = DHCP_run();
        if (g_dhcp_ip_found) return 0;
        if (dhcp_state == DHCP_FAILED) return -2;
        if (dhcp_state == DHCP_STOPPED) return -3;
        sleep_ms(10);
    }

    return -4;
}


static int W5500_Ethernet_Link(uint32_t timeout_ms){
    uint8_t link = PHY_LINK_OFF;
    uint32_t elapsed_ms = 0;

    while (elapsed_ms < timeout_ms) {
        if (ctlwizchip(CW_GET_PHYLINK, &link) == -1) return -1;
        if (link == PHY_LINK_ON) return 0;

        sleep_ms(W5500_LINK_POLL_MS);
        elapsed_ms += W5500_LINK_POLL_MS;
    }

    return -2;
}


int W5500_Connect(void){
    if (!g_board_initialized) return -1;
    if (!g_conn_initialized) return -2;
    if (W5500_Ethernet_Link(W5500_LINK_TIMEOUT_MS) != 0) return -3;

    if (g_conn.use_dhcp) {
        if (W5500_DHCP_Connect(W5500_DHCP_DEFAULT_TIMEOUT_MS) != 0) return -4;
        return 0;
    }
    if (ctlnetwork(CN_SET_NETINFO, &g_netinfo) == -1) return -5;

    return 0;
}


static bool W5500_ServerConfig_IsValid(const W5500_Network_Config_t *cfg){
    if(cfg == NULL) return false;
    if (!(cfg->config_flags & W5500_CFG_FLAG_SERVER_CONFIGURED)) return false;
    if (!w5500_is_valid_ipv4_addr(cfg->server_ip)) return false;
    if (cfg->server_port == 0) return false;
    if (cfg->http_path[0] == '\0') return false;

    return true;

}


static bool W5500_Is_Config_Valid(const W5500_Network_Config_t *cfg){
    if (cfg == NULL) return false;
    if(cfg->magic != W5500_CONFIG_MAGIC) return false;
    if(!w5500_is_valid_crc(cfg)) return false;
    if(!w5500_is_valid_mac(cfg->mac)) return false;
    if(cfg-> interval_s < W5500_MIN_INTERVAL_S  || cfg-> interval_s > W5500_MAX_INTERVAL_S) return false; 
    if(!cfg->use_dhcp){
        if (!w5500_is_valid_ipv4_addr(cfg->ip)) return false;
        if (!w5500_is_valid_netmask(cfg->sn)) return false;
    }
    
    if((cfg->config_flags & W5500_CFG_FLAG_SERVER_CONFIGURED) != 0){
        if (!W5500_ServerConfig_IsValid(cfg)) return false;
    }

    return true;
}


static bool W5500_Is_Config_ReadytoSave(const W5500_Network_Config_t *cfg){
    if (cfg == NULL) return false;
    if (!w5500_is_valid_mac(cfg->mac)) return false;
    if (cfg->interval_s < W5500_MIN_INTERVAL_S || cfg->interval_s > W5500_MAX_INTERVAL_S) return false;
    if (!cfg->use_dhcp) {
        if (!w5500_is_valid_ipv4_addr(cfg->ip)) return false;
        if (!w5500_is_valid_netmask(cfg->sn)) return false;
    }
    if ((cfg->config_flags & W5500_CFG_FLAG_SERVER_CONFIGURED) != 0) {
        if (!W5500_ServerConfig_IsValid(cfg)) return false;
    }

    return true;
}


int W5500_SaveConfig(const W5500_Network_Config_t *cfg){
    if (cfg == NULL) return -1;
    if (!W5500_Is_Config_ReadytoSave(cfg)) return -2;
    if ((CONFIG_FLASH_OFFSET % FLASH_SECTOR_SIZE) != 0) return -3;

    W5500_Network_Config_t local_cfg = *cfg;
    local_cfg.magic = W5500_CONFIG_MAGIC;
    local_cfg.crc = 0;
    local_cfg.crc = w5500_crc32_compute( &local_cfg, offsetof(W5500_Network_Config_t, crc));

    enum {
        CONFIG_PROGRAM_SIZE = ((sizeof(W5500_Network_Config_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE
    };

    uint8_t flash_buffer[CONFIG_PROGRAM_SIZE];
    memset(flash_buffer, 0xFF, sizeof(flash_buffer));
    memcpy(flash_buffer, &local_cfg, sizeof(local_cfg));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, flash_buffer, sizeof(flash_buffer));
    restore_interrupts(ints);

    return 0;
}


W5500_Config_Result_t  W5500_LoadOrInitNetworkConfig(W5500_Network_Config_t *cfg){
    if(cfg == NULL) return W5500_CONFIG_RESULT_ERROR_NOT_PASSED;
    if(W5500_Load_Flash_Config(cfg) == 0){
        if(W5500_Is_Config_Valid(cfg)) return W5500_CONFIG_RESULT_LOADED;
    }
    W5500_Default_config(cfg);
    
    return W5500_CONFIG_DEFAULT_INITIALIZED;
}


int W5500_GetCurrentConfig(W5500_Network_Config_t *out_cfg) {
    if (out_cfg == NULL) return -1;
    if (!g_conn_initialized) return -2;

    *out_cfg = g_conn;
    return 0;
}


void W5500_PrintCurrentConfig(void) {
    W5500_Network_Config_t cfg;

    if (W5500_GetCurrentConfig(&cfg) != 0) {
        printf("W5500 config not initialized\r\n");
        return;
    }

    printf("device_id: %s\r\n", cfg.device_id);
    printf("mac: %02X:%02X:%02X:%02X:%02X:%02X\r\n", cfg.mac[0], cfg.mac[1], cfg.mac[2], cfg.mac[3], cfg.mac[4], cfg.mac[5]);
    printf("use_dhcp: %d\r\n", cfg.use_dhcp);
    printf("server_ip: %u.%u.%u.%u\r\n", cfg.server_ip[0], cfg.server_ip[1], cfg.server_ip[2], cfg.server_ip[3]);
    printf("server_port: %u\r\n", cfg.server_port);
    printf("http_path: %s\r\n", cfg.http_path);
    printf("interval_s: %lu\r\n", (unsigned long)cfg.interval_s);
    printf("config_flags: 0x%08lX\r\n", (unsigned long)cfg.config_flags);
}


void W5500_PrintChipNetworkInfo(void) {
    wiz_NetInfo info;
    memset(&info, 0, sizeof(info));

    ctlnetwork(CN_GET_NETINFO, &info);
    printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n", info.mac[0], info.mac[1], info.mac[2], info.mac[3], info.mac[4], info.mac[5]);
    printf("IP: %u.%u.%u.%u\r\n", info.ip[0], info.ip[1], info.ip[2], info.ip[3]);
    printf("SUBNET: %u.%u.%u.%u\r\n", info.sn[0], info.sn[1], info.sn[2], info.sn[3]);
    printf("GATEWAY: %u.%u.%u.%u\r\n", info.gw[0], info.gw[1], info.gw[2], info.gw[3]);
    printf("DNS: %u.%u.%u.%u\r\n", info.dns[0], info.dns[1], info.dns[2], info.dns[3]);
    printf("DHCP mode: %s\r\n", info.dhcp == NETINFO_DHCP ? "DHCP" : "STATIC");
}


static int UDP_BuildDiscover(const W5500_Network_Config_t *cfg, uint8_t *buffer, size_t buffer_size) {
    if (cfg == NULL || buffer == NULL || buffer_size == 0) return -1;

    int len = snprintf((char *)buffer, buffer_size,
        "DISCOVER freezer_sensor_v1 mac=%02X:%02X:%02X:%02X:%02X:%02X fw=0.1.0 hw=w5500-pico",
        cfg->mac[0], cfg->mac[1], cfg->mac[2], cfg->mac[3], cfg->mac[4], cfg->mac[5]
    );
    
    if (len < 0) return -2;
    if ((size_t)len >= buffer_size) return -3;

    return len;
}


static int UDP_ParseAddress(const char *text, uint8_t ip[4]){
    if (text == NULL || ip == NULL) return -1;

    unsigned int a, b, c, d;
    if (sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -2;
    if (a > 255 || b > 255 || c > 255 || d > 255) return -3;

    ip[0]= (uint8_t)a;
    ip[1]= (uint8_t)b;
    ip[2]= (uint8_t)c;
    ip[3]= (uint8_t)d;

    return 0;
}


static int UDP_ExtractValue(const char *text, const char *key, char *out, size_t out_size){
    if (text == NULL || key == NULL || out == NULL || out_size == 0) return -1;

    const char *start = strstr(text, key);
    if (start == NULL) return -2;
    start += strlen(key);

    const char *end = strchr(start, ' ');
    size_t len;

    if (end != NULL) {
        len = (size_t)(end - start);
    } else {
        len = strlen(start);
    }

    if (len == 0) return -3;
    if (len >= out_size) return -4;

    memcpy(out, start, len);
    out[len] = '\0';

    return 0;
}


//example response: CONFIG freezer_sensor_v1 device_id=freezer-test-001 server_ip=192.168.1.100 server_port=8090 http_path=/measurement interval_s=10
static int UDP_ParseDiscovery(const char *response, const uint8_t remote_ip[4], W5500_Network_Config_t *cfg){
    if (response == NULL || cfg == NULL) return -1;
    if (strstr(response, "CONFIG freezer_sensor_v1") == NULL) return -2;

    char device_id[32];
    char server_ip_text[24];
    char server_port_text[8];
    char http_path[64];
    char interval_text[12];
    uint8_t server_ip[4];
    unsigned long server_port_ul;
    unsigned long interval_ul;

    if(UDP_ExtractValue(response, "device_id=", device_id, sizeof(device_id)) != 0) return -3;
    if(UDP_ExtractValue(response, "server_ip=", server_ip_text, sizeof(server_ip_text)) != 0) return -4;
    if(UDP_ParseAddress(server_ip_text, server_ip) != 0) return -5;
    if(UDP_ExtractValue(response, "server_port=", server_port_text, sizeof(server_port_text)) != 0) return -6;

    server_port_ul = strtoul(server_port_text, NULL, 10);
    if(server_port_ul == 0 || server_port_ul > 65535) return -7;
    if(UDP_ExtractValue(response, "http_path=", http_path, sizeof(http_path)) != 0) return -8;
    if(http_path[0] != '/') return -9;
    if(UDP_ExtractValue(response, "interval_s=", interval_text, sizeof(interval_text)) != 0) return -10;

    interval_ul = strtoul(interval_text, NULL, 10);
    if(interval_ul < W5500_MIN_INTERVAL_S || interval_ul > W5500_MAX_INTERVAL_S) return -11;

    memset(cfg->device_id, 0, sizeof(cfg->device_id));
    strncpy(cfg->device_id, device_id, sizeof(cfg->device_id) - 1);
    memcpy(cfg->server_ip, server_ip, 4);
    cfg->server_port = (uint16_t)server_port_ul;
    memset(cfg->http_path, 0, sizeof(cfg->http_path));
    strncpy(cfg->http_path, http_path, sizeof(cfg->http_path) - 1);
    cfg->interval_s = (uint32_t)interval_ul;
    cfg->config_flags |= W5500_CFG_FLAG_SERVER_CONFIGURED;
    cfg->config_flags |= W5500_CFG_FLAG_PROVISIONED;

    return 0;
}


int W5500_UDP_Discovery(W5500_Network_Config_t *cfg){
    if (cfg == NULL) return -1;
    if (!w5500_is_valid_mac(cfg->mac)) return -2;

    close(SOCK_DISCOVERY);
    sleep_ms(10);

    int8_t r = socket(SOCK_DISCOVERY, Sn_MR_UDP, DISCOVERY_LOCAL_PORT, 0);
    if (r != SOCK_DISCOVERY) return -3;

    uint8_t sr = getSn_SR(SOCK_DISCOVERY);
    if (sr != SOCK_UDP) {
        close(SOCK_DISCOVERY);
        return -4;
    }

    int msg_len = UDP_BuildDiscover(cfg, g_udp_discover_buffer, sizeof(g_udp_discover_buffer));
    if (msg_len < 0) {
        close(SOCK_DISCOVERY);
        return -5;
    }

    //uint8_t target_ip[4] = {192, 168, 1, 100};
    uint8_t target_ip[4] = {255, 255, 255, 255};

    for (int attempt = 1; attempt <= DISCOVERY_MAX_ATTEMPTS; attempt++) {
        printf("Sending UDP DISCOVER to %u.%u.%u.%u:%u\r\n", target_ip[0], target_ip[1], target_ip[2], target_ip[3], DISCOVERY_SERVER_PORT);

        int32_t sent = sendto(SOCK_DISCOVERY, g_udp_discover_buffer, (uint16_t)msg_len, target_ip, DISCOVERY_SERVER_PORT);
        if (sent != msg_len) {
            printf("sendto failed on attempt %d\r\n", attempt);
            sleep_ms(100);
            continue;
        }

        absolute_time_t start = get_absolute_time();
        while (absolute_time_diff_us(start, get_absolute_time()) < (int64_t)DISCOVERY_RESPONSE_TIMEOUT_MS * 1000) {

            uint16_t rx_size = getSn_RX_RSR(SOCK_DISCOVERY);
            if (rx_size > 0) {
                if (rx_size >= sizeof(g_udp_discover_buffer)) rx_size = sizeof(g_udp_discover_buffer) - 1;

                uint8_t remote_ip[4] = {0};
                uint16_t remote_port = 0;

                int32_t received = recvfrom(SOCK_DISCOVERY, g_udp_discover_buffer, rx_size, remote_ip, &remote_port);
                if (received > 0) {
                    g_udp_discover_buffer[received] = '\0';
                    printf("UDP response: %s\r\n", (char *)g_udp_discover_buffer);

                    int parse_ret = UDP_ParseDiscovery((char *)g_udp_discover_buffer, remote_ip, cfg );
                    if (parse_ret == 0 && W5500_ServerConfig_IsValid(cfg)) {
                        close(SOCK_DISCOVERY);
                        return 0;
                    }

                    printf("Invalid CONFIG response, ignoring\r\n");
                }
                break;
            }
            sleep_ms(DISCOVERY_POLL_MS);
        }
        sleep_ms(200);
    }

    printf("UDP discovery failed: no response\r\n");

    close(SOCK_DISCOVERY);
    return -6;
}


static int W5500_UpdateServerConfig(bool force_discovery) {
    if (!g_conn_initialized) return -1;
    if (!force_discovery &&
        W5500_ServerConfig_IsValid(&g_conn)) {
        return 0;
    }

    int ret = W5500_UDP_Discovery(&g_conn);
    if (ret != 0) {
        printf("UDP Discovery failed: %d\r\n", ret);
        stdio_flush();
        return -2;
    }

    if (!W5500_ServerConfig_IsValid(&g_conn)) {
        printf("Invalid server config after UDP Discovery\r\n");
        stdio_flush();
        return -3;
    }

    ret = W5500_SaveConfig(&g_conn);
    if (ret != 0) {
        printf("Saving config failed: %d\r\n", ret);
        stdio_flush();
        return -4;
    }

    return 0;
}


int W5500_ResolveServerConfig(void) {
    return W5500_UpdateServerConfig(false);
}


int W5500_RefreshServerConfig(void) {
    return W5500_UpdateServerConfig(true);
}


static uint16_t W5500_HTTP_NextLocalPort(void){
    if (g_http_local_port > 60000) g_http_local_port = 50000;
    return g_http_local_port++;
}


static int W5500_CloseSocket(uint8_t sn, uint32_t timeout_ms){
    disconnect(sn);
    close(sn);

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    while (getSn_SR(sn) != SOCK_CLOSED) {
        if (time_reached(deadline)) return -1;
        sleep_ms(1);
    }

    return 0;
}


W5500_HTTP_Result_t W5500_HTTP_POST_JSON(const char *endpoint, const char *payload){
    const uint8_t sn = SOCK_HTTP;

    char request[512];
    uint8_t rx_buf[128 + 1];

    if (payload == NULL) return W5500_HTTP_ERR_NULL_PAYLOAD;
    if (!W5500_ServerConfig_IsValid(&g_conn)) return W5500_HTTP_ERR_INVALID_CONFIG;

    const char *path = endpoint;
    if (path == NULL || path[0] == '\0') path = g_conn.http_path;

    const size_t payload_len = strlen(payload);
    const int req_len = snprintf(
        request,
        sizeof(request),
        "POST %s HTTP/1.1\r\n"
        "Host: %u.%u.%u.%u:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path,
        g_conn.server_ip[0],
        g_conn.server_ip[1],
        g_conn.server_ip[2],
        g_conn.server_ip[3],
        g_conn.server_port,
        (unsigned)payload_len,
        payload
    );

    if (req_len < 0 || req_len >= (int)sizeof(request)) return W5500_HTTP_ERR_REQUEST_TOO_LARGE;
    if (W5500_CloseSocket(sn, 100) != 0) return W5500_HTTP_ERR_SOCKET_CLOSE;

    const uint16_t local_port = W5500_HTTP_NextLocalPort();
    const int8_t socket_ret = socket(sn, Sn_MR_TCP, local_port, 0);
    if (socket_ret != sn) {
        close(sn);
        return W5500_HTTP_ERR_SOCKET_OPEN;
    }

    const int8_t connect_ret = connect(sn, g_conn.server_ip, g_conn.server_port);
    printf("HTTP connect: ret=%d status=0x%02X ir=0x%02X port=%u\r\n", connect_ret, getSn_SR(sn), getSn_IR(sn), local_port);
    stdio_flush();

    if (connect_ret != SOCK_OK) {
        close(sn);
        return W5500_HTTP_ERR_CONNECT;
    }

    int sent_total = 0;
    while (sent_total < req_len) {
        const int32_t sent = send(sn, (uint8_t *)&request[sent_total], req_len - sent_total);
        if (sent <= 0) {
            close(sn);
            return W5500_HTTP_ERR_SEND;
        }
        sent_total += sent;
    }

    const absolute_time_t deadline = make_timeout_time_ms(W5500_HTTP_TIMEOUT_MS);

    while (getSn_RX_RSR(sn) == 0) {
        const uint8_t status = getSn_SR(sn);

        if (status == SOCK_CLOSED) {
            close(sn);
            return W5500_HTTP_ERR_REMOTE_CLOSED;
        }

        if (time_reached(deadline)) {
            close(sn);
            return W5500_HTTP_ERR_RESPONSE_TIMEOUT;
        }

        sleep_ms(1);
    }

    uint16_t available = getSn_RX_RSR(sn);
    if (available > sizeof(rx_buf) - 1) available = sizeof(rx_buf) - 1;

    const int32_t received = recv(sn, rx_buf, available);
    if (received <= 0) {
        close(sn);
        return W5500_HTTP_ERR_RECEIVE;
    }

    rx_buf[received] = '\0';
    close(sn);

    if (strstr((char *)rx_buf, "HTTP/1.1 200") != NULL || strstr((char *)rx_buf, "HTTP/1.0 200") != NULL ||
        strstr((char *)rx_buf, "HTTP/1.1 201") != NULL || strstr((char *)rx_buf, "HTTP/1.0 201") != NULL ||
        strstr((char *)rx_buf, "HTTP/1.1 204") != NULL || strstr((char *)rx_buf, "HTTP/1.0 204") != NULL
    ) return W5500_HTTP_OK;

    return W5500_HTTP_ERR_STATUS_CODE;
}