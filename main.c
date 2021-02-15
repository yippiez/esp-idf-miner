
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "mbedtls/md.h"
#include "mbedtls/sha1.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"

////////////////////////////////////////////////

/* Wifi Settings */
#define SSID "SSIDname"
#define PASS "password"
#define MAX_RETRY 10

/* Event Group For Detecting Established Connection */
static EventGroupHandle_t s_wifi_event_group;
static int retry_num = 0;

#define WIFI_CONNECTED_BIT BIT0 // Connected to acces point with ip ( 1 << 0 )
#define WIFI_FAIL_BIT      BIT1 // Failed connectio                 ( 1 << 1 )

/* GPIO Settings */
#define BLINK_GPIO 2

/* Miner Settings */
#define UNAME ""
#define POOL_ADDRESS ""
#define POOL_PORT 0

static unsigned long accepted_shares = 0;
static unsigned long rejected_shares = 0;

///////////////////////////////////////////////

void gpio_setup(){
    
    /* IOMUX */
    gpio_pad_select_gpio(BLINK_GPIO);
    
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
 
}

void blink(uint8_t n){

    while(n--){
        // led gpio set low
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        // led gpio set high
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

}

static void event_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data) {

        if(event_base == WIFI_EVENT){

                
                if (event_id == WIFI_EVENT_STA_START){
                    esp_wifi_connect();
                }

                if (event_id == WIFI_EVENT_STA_DISCONNECTED){

                    printf("Can't connect to station\n");
                
                    if(retry_num < MAX_RETRY){
                        // reconnect when failed connection
                        esp_wifi_connect();
                        retry_num++;
                        printf("Reconnecting...\n");
                    }else{
                        // max reconnect count reached
                        printf("Max reconnect reached failed connection!\n");
                        // setting the fail bit
                        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    }

                }

        }

        if(event_base == IP_EVENT){

            if(event_id == IP_EVENT_STA_GOT_IP){
                ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
                printf("Got IP: %d.%d.%d.%d on AP: %s\n", IP2STR(&event->ip_info.ip), SSID);                
                retry_num = 0;
                xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            }

        }

}

void wifi_init_sta(){

    /* Event Group */    
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Netif initialization */
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Event Group */ 
    // https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/esp_event.html
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    
    // register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));


    /* Wifi Config */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = SSID,
            .password = PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            }

        }
    };

    /* Start Station Mode */

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    printf("Station mode init finished.\n");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, // event group
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, // bits to wait for
        pdFALSE, // wait until all bits are set
        pdFALSE, // clear bits on exit
        portMAX_DELAY); // ticks to wait ( max )


    if(bits & WIFI_CONNECTED_BIT)
        printf("Succesfully connected to: %s\n", SSID);
    else if(bits & WIFI_FAIL_BIT)
        printf("Connection attempt unsuccesfull\n");
    else
        printf("Unexpected connection neither failed nor connected\n");

    // unregister event handlers
    esp_event_handler_instance_unregister(WIFI_EVENT,
                                ESP_EVENT_ANY_ID,
                                instance_any_id);
    
    esp_event_handler_instance_unregister(IP_EVENT,
                                        IP_EVENT_STA_GOT_IP,
                                        instance_got_ip);

    // delete the group handle
    vEventGroupDelete(s_wifi_event_group);

}

void miner( void * pvParameters ){
    
    char host_ip[] = POOL_ADDRESS;
    char rx_buf[512];
    int sock, len;

    while(1){
        
        /* Socket Config */
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = inet_addr(host_ip);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(POOL_PORT);

        /* Creatin The Socket */
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        
        if(sock < 0){
            printf("Unable to create a socket: errno:%d\n", errno);
            break;
        }

        int err = connect(sock, (struct sockaddr *) &dest_addr, sizeof(struct sockaddr_in));

        if( err != 0){
            printf("Unable to connect to pool errno:%d\n", errno);
            break;
        }

        printf("Miner connected succesfully\n");
        blink(3);

        /* Receiving Server version */
        len = recv(sock, rx_buf, 4, 0);

        // error occured
        if(len < 0){
            printf("Error when receiving errno:%d", errno);
            break;
        
        }else{
        
            rx_buf[len] = 0;
            printf("Server is on version: %s", rx_buf);
        }

        while(1){
            /* Request job from pool */
            
            err = send(sock, "JOB," UNAME ",ESP", 4 + sizeof(UNAME), 0); // maybe sizeof(uname) - 1

            if(err != 0){
                printf("Error when getting job restarting socket errno:%d\n", errno);
                break;
            }

            len = recv(sock, rx_buf, 512, 0);

            if(len < 0){
                printf("Error when receiving job. Reconnecting socket errno:%d", errno);
                break;
            }else{

                // received job
                rx_buf[len] = 0;

                char *hash  = strtok(rx_buf, ",");
                char *goal = strtok(NULL  , ",");
                int i_diff = 7501;
                
                printf("DEBUG: Got a hash:%s, And a goal:%s", hash, goal);

                size_t hash_length = strlen(hash);
                char hash_buf[hash_length + 5];

                for(int i=0; i<(i_diff * 100); i++){
                    
                    snprintf(hash_buff, hash_length + 5, "%s%d", hash, i);

                    /*
                        HASH CHECKING ALGO
                        AND SUBMITTING

                    */
                }


            }

        }

        if(sock == -1){
            printf("Shutting down socket\n");
            shutdown(sock, 0);
            close(sock);
        }

    }


}

void hash_monitor(){

    printf("Accepted Shares:%lu Rejected Shares:%lu\n", accepted_shares, rejected_shares);

}

void app_main() {
    
    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Setup GPIO */
    gpio_setup();
   
    /* Connect To Wifi */
    gpio_set_level(BLINK_GPIO, 1); // turn on led
    vTaskDelay(300 / portTICK_PERIOD_MS);
    wifi_init_sta();
    gpio_set_level(BLINK_GPIO, 0); // turn of led
    vTaskDelay(300 / portTICK_PERIOD_MS);

    blink(2);

    miner();
}