
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "driver/i2c.h"
#include "esp_sntp.h"
#include "i2c_lcd.h"


#define WIFI_SSID "A25"
#define WIFI_PASS "123456789"

#define MQTT_BROKER "mqtt://broker.hivemq.com"
#define MQTT_TOPIC  "esp32/sleep_status"
#define MQTT_CLIENT_ID "esp32_client_12345"
#define BTN_GPIO  GPIO_NUM_0
#define LED_GPIO  GPIO_NUM_4
#define BUZZER_PIN  GPIO_NUM_2
#define RESET_GPIO GPIO_NUM_5
volatile uint32_t last_btn_time = 0;
volatile uint32_t last_reset_time = 0;
volatile float level = 0;

uint32_t last_sleep_time = 0; 
SemaphoreHandle_t reset_semaphore;   
SemaphoreHandle_t state_mutex;       
SemaphoreHandle_t text_mutex;
#define MAX_HISTORY 5
typedef struct {
    int day;
    int month;
    int hour;
    int min;
} sleep_time_t;
sleep_time_t sleep_history[MAX_HISTORY];
int head_idx = 0;  
int count_history = 0; 
static const char *TAG = "APP";
char queue_text[512];



typedef enum {
    STATE_NORMAL,
    STATE_WARNING,
    STATE_REMIND
} system_state_t;

volatile system_state_t STATE = STATE_NORMAL;

#define EVT_MQTT_SLEEP  (1 << 0)
#define EVT_MQTT_AWAKE  (1 << 1)
#define EVT_BUTTON      (1 << 2)
#define EVT_TIMER1  (1 << 3)
#define EVT_TIMER2  (1 << 4)
#define EVT_SHOW_QUEUE  (1 << 5)


TaskHandle_t state_task_handle;

TimerHandle_t timer1;   // 3s → WARNING
TimerHandle_t timer2;   // 5s → NORMAL

esp_mqtt_client_handle_t client;





void get_time_str(char *buffer)
{
    time_t now;
    struct tm timeinfo;

    time(&now);
    localtime_r(&now, &timeinfo);

    sprintf(buffer, "%02d:%02d:%02d",
            timeinfo.tm_hour,
            timeinfo.tm_min,
            timeinfo.tm_sec);
}
float compute_level()
{
    uint32_t now = xTaskGetTickCount();

    float dt_min = (now - last_sleep_time) / 60000.0f; 

    float decay_rate = 0.2f; 

    float current = level - (dt_min * decay_rate);

    if (current < 0) current = 0;

    return current;
}
sleep_time_t get_current_time()
{
    time_t now;
    struct tm timeinfo;
    sleep_time_t t;

    time(&now);
    localtime_r(&now, &timeinfo);

    t.day  = timeinfo.tm_mday;
    t.month = timeinfo.tm_mon + 1;
    t.hour = timeinfo.tm_hour;
    t.min  = timeinfo.tm_min;

    return t;
}

void add_to_history(sleep_time_t t) {
    xSemaphoreTake(text_mutex, portMAX_DELAY);
    
    sleep_history[head_idx] = t;
    head_idx = (head_idx + 1) % MAX_HISTORY; // Quay vòng chỉ số
    
    if (count_history < MAX_HISTORY) {
        count_history++;
    }
    
    xSemaphoreGive(text_mutex);
}

void update_history_text() {
    char msg[256] = "";
    int len = 0;

    xSemaphoreTake(text_mutex, portMAX_DELAY);
    
    // Duyệt từ phần tử mới nhất đến cũ nhất (tùy bạn chọn thứ tự)
    for (int i = 0; i < count_history; i++) {
        // Tính toán chỉ số thực tế trong mảng vòng
        int idx = (head_idx - 1 - i + MAX_HISTORY) % MAX_HISTORY;
        
        len += snprintf(msg + len, sizeof(msg) - len,
                        "%02d/%02d %02d:%02d\n",
                        sleep_history[idx].day, sleep_history[idx].month,
                        sleep_history[idx].hour, sleep_history[idx].min);
    }
    strncpy(queue_text, msg, sizeof(queue_text));
    queue_text[sizeof(queue_text) - 1] = '\0';
    xSemaphoreGive(text_mutex);
}




void time_init(void)
{
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    setenv("TZ", "ICT-7", 1);
    tzset();
}
void IRAM_ATTR reset_isr(void *arg)
{
    BaseType_t hpw = pdFALSE;
        uint32_t now = xTaskGetTickCountFromISR();

    if ((now - last_reset_time) < pdMS_TO_TICKS(200)) {
        return;
    }

    last_reset_time = now;


    xSemaphoreGiveFromISR(reset_semaphore, &hpw);

    portYIELD_FROM_ISR(hpw);
}
void reset_task(void *pvParameters)
{
    while (1)
    {
        // chờ ISR signal
        if (xSemaphoreTake(reset_semaphore, portMAX_DELAY) == pdTRUE)
        {
            ESP_LOGW(TAG, "RESET → NORMAL");
            xTimerStop(timer1, 0);
            xTimerStop(timer2, 0);
            xSemaphoreTake(state_mutex, portMAX_DELAY);
            STATE = STATE_NORMAL;
            xSemaphoreGive(state_mutex);
        }
    }
}
void IRAM_ATTR button_isr(void *arg)
{
    BaseType_t hpw = pdFALSE;
        uint32_t now = xTaskGetTickCountFromISR();

    // debounce 200ms
    if ((now - last_btn_time) < pdMS_TO_TICKS(200)) {
        return;
    }

    last_btn_time = now;

    xTaskNotifyFromISR(
        state_task_handle,
        EVT_BUTTON,
        eSetBits,
        &hpw
    );

    portYIELD_FROM_ISR(hpw);
}

void gpio_init_all(void)
{
    gpio_config_t io_conf = {};

  
    io_conf.pin_bit_mask = (1ULL << LED_GPIO);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    io_conf.pin_bit_mask = (1ULL << BUZZER_PIN);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    io_conf.pin_bit_mask = (1ULL << BTN_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&io_conf);

    io_conf.pin_bit_mask = (1ULL << RESET_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(BTN_GPIO, button_isr, (void*) BTN_GPIO);
    gpio_isr_handler_add(RESET_GPIO, reset_isr, (void*) RESET_GPIO);
}
void publish_queue_plaintext()
{
char local_buf[512];
xSemaphoreTake(text_mutex, portMAX_DELAY);
strncpy(local_buf, queue_text, sizeof(local_buf));
xSemaphoreGive(text_mutex);
local_buf[sizeof(local_buf) - 1] = '\0';

esp_mqtt_client_publish(
    client,
    MQTT_TOPIC,
    local_buf,
    0,
    1,
    0
);
}

void timer1_cb(TimerHandle_t xTimer)
{
    xTaskNotify(state_task_handle, EVT_TIMER1, eSetBits);
}

void timer2_cb(TimerHandle_t xTimer)
{
    xTaskNotify(state_task_handle, EVT_TIMER2, eSetBits);
}


static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    if (event->event_id == MQTT_EVENT_CONNECTED)
    {
        ESP_LOGI(TAG, "MQTT Connected");
        esp_mqtt_client_subscribe(client, MQTT_TOPIC, 1);
    }

    else if (event->event_id == MQTT_EVENT_DATA)
    {
        char msg[16] = {0};
        memcpy(msg, event->data, event->data_len);

        uint32_t notify = 0;
        if (strcmp(msg, "SLEEP") == 0)
            notify = EVT_MQTT_SLEEP;

        else if (strcmp(msg, "AWAKE") == 0)
            notify = EVT_MQTT_AWAKE;
        else if (strcmp(msg, "SHOW") == 0)   
            notify = EVT_SHOW_QUEUE;

        if (notify)
            xTaskNotify(state_task_handle, notify, eSetBits);
    }
}


void start_mqtt(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_BROKER,
        .credentials.client_id = MQTT_CLIENT_ID,
        .buffer.size = 1024, 
        .buffer.out_size = 1024,
    };

    client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

static void wifi_event_handler(void* arg,
                               esp_event_base_t base,
                               int32_t event_id,
                               void* data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ESP_LOGI(TAG, "WiFi OK → MQTT start");
        time_init();   
        start_mqtt();
    }
}
void wait_for_time()
{
    time_t now = 0;
    struct tm timeinfo = {0};

    int retry = 0;

    while (timeinfo.tm_year < (2020 - 1900) && retry < 10)
    {
        ESP_LOGI("TIME", "Waiting for time sync...");
        vTaskDelay(pdMS_TO_TICKS(1000));

        time(&now);
        localtime_r(&now, &timeinfo);

        retry++;
    }
}
void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();
}

void state_task(void *pvParameters)
{
    uint32_t notifyValue;

    while (1)
    {
        xTaskNotifyWait(
            0xFFFFFFFF,
            0xFFFFFFFF,
            &notifyValue,
            portMAX_DELAY
        );

   
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        if ((notifyValue & EVT_MQTT_SLEEP)&&(STATE == STATE_NORMAL))
        {
            ESP_LOGI(TAG, "MQTT SLEEP");

            xTimerStart(timer1, 0);   // 3s → WARNING
         
        }
        xSemaphoreGive(state_mutex);
        if (notifyValue & EVT_MQTT_AWAKE)
        {
            ESP_LOGI(TAG, "MQTT AWAKE");
            xTimerStop(timer1,0);
          // 5s → NORMAL
        }

       
        if (notifyValue & EVT_BUTTON)
        {
            xSemaphoreTake(state_mutex, portMAX_DELAY);
            if (STATE == STATE_WARNING)
            {
        
                STATE = STATE_REMIND;
                ESP_LOGW(TAG, "STATE → REMIND");

                xTimerStart(timer2, 0);
            }
            xSemaphoreGive(state_mutex);
        }
        if (notifyValue & EVT_TIMER1)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    STATE = STATE_WARNING;
    xSemaphoreGive(state_mutex);
    level += 1;
    last_sleep_time = xTaskGetTickCount();
    ESP_LOGW(TAG, "TIMER1 → STATE WARNING");
    esp_mqtt_client_publish(
    client,
    MQTT_TOPIC,
    "WARNING",
    0,
    1,
    0
);
    add_to_history(get_current_time());
    update_history_text();
}

if (notifyValue & EVT_TIMER2)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    STATE = STATE_NORMAL;
    xSemaphoreGive(state_mutex);
    ESP_LOGI(TAG, "TIMER2 → STATE NORMAL");
        esp_mqtt_client_publish(
    client,
    MQTT_TOPIC,
    "NORMAL",
    0,
    1,
    0
);
}
if (notifyValue & EVT_SHOW_QUEUE)
{
publish_queue_plaintext();
}
    }
}
void lcd_task(void *pvParameters)
{
    char buffer[20];
    while (1)
    {

        if (STATE == STATE_NORMAL)
        {
            lcd_clear();
            lcd_put_cursor(0, 0);
            lcd_send_string("Status: NORMAL");
            
        }
        else if (STATE == STATE_WARNING)
        {
            lcd_clear();
            lcd_put_cursor(0, 0);
            lcd_send_string("Status: WARNING");
            
        }
        else if (STATE == STATE_REMIND)
        {
            lcd_clear();
            lcd_put_cursor(0, 0);
            lcd_send_string("Status: REMIND");
            
        }
        get_time_str(buffer);
        lcd_put_cursor(1, 0);
        lcd_send_string("Time: ");
        lcd_send_string(buffer);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void buzzer_task(void *pvParameters)
{

    while (1)
    {
        if (STATE == STATE_WARNING)
        {
            uint32_t ms_time;
        level = compute_level();
        if (level > 5)
        {
            ms_time = 100;
        }
        else if (level<=0){
            ms_time = 600;
        }
        else ms_time=100*(5-level);
            gpio_set_level(BUZZER_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(ms_time));
            gpio_set_level(BUZZER_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(ms_time));
        }
        else if (STATE == STATE_REMIND)
        {
            gpio_set_level(BUZZER_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        else
        {
            gpio_set_level(BUZZER_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}
void led_task(void *pvParameters)
{
    while (1)
    {
       
        if (STATE == STATE_WARNING)
        {
        uint32_t ms_time;
        level = compute_level();
        if (level > 5)
        {
            ms_time = 100;
        }
        else if (level<=0){
            ms_time = 600;
        }
        else ms_time=100*(5-level);
            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(ms_time));
            gpio_set_level(LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(ms_time));
        }
        else if (STATE == STATE_REMIND)
        {
            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        else
        {
            gpio_set_level(LED_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}


void app_main(void)
{
    nvs_flash_init();
    gpio_init_all();
    wifi_init();
    wait_for_time();
    lcd_init();                          
    lcd_clear();                            

    text_mutex = xSemaphoreCreateMutex();
    reset_semaphore = xSemaphoreCreateBinary();
    state_mutex = xSemaphoreCreateMutex();
    timer1 = xTimerCreate("t1", pdMS_TO_TICKS(3000),
                          pdFALSE, NULL, timer1_cb);

    timer2 = xTimerCreate("t2", pdMS_TO_TICKS(5000),
                          pdFALSE, NULL, timer2_cb);
    // TASK
    xTaskCreate(state_task,
                "state_task",
                8192,
                NULL,
                5,
                &state_task_handle);

    xTaskCreate(buzzer_task,
                "buzzer_task",
                2048,
                NULL,
                5,
                NULL);
    xTaskCreate(led_task,
            "led_task",
            2048,
            NULL,
            5,
            NULL);
    xTaskCreate(lcd_task,
                "lcd_task",
                2048,
                NULL,
                5,
                NULL);

xTaskCreate(reset_task,
            "reset_task",
            2048,
            NULL,
            6,
            NULL);


}