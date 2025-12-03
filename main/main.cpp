#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "camera_pins.h"
#include "driver/gpio.h"
#include "face_recognition.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "led_strip.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// RGB LED PIN
#define WS2812_PIN 48
led_strip_handle_t led_strip;

static const char *TAG = "camera_http_server";

// WiFi credentials - CHANGE THESE (search config.url webhook as well and replace with your webhook)
#define WIFI_SSID ""
#define WIFI_PASS ""

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
static SemaphoreHandle_t camera_mutex = NULL;
char name[MAX_NAME_LENGTH] = "Unknown";
static char last_sent_name[MAX_NAME_LENGTH] = "";
static TickType_t last_recognition_time = 0;
static SemaphoreHandle_t name_mutex = NULL;

#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048
#define RECOGNITION_INTERVAL_MS 2000  // Check for faces every 2 seconds
#define RECOGNITION_COOLDOWN_MS 30000 // Wait 30 seconds before sending same name again

// Forward declarations
void discord_task(void *param);
bool sendDiscordMessage(const char* message);
void face_recognition_task(void *param);

// HTML page for face enrollment
static const char* index_html = "<!DOCTYPE html>"
"<html>"
"<head>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>Face Recognition Camera</title>"
"<style>"
"body { font-family: Arial; margin: 20px; background: #f0f0f0; }"
".container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; }"
"h1 { color: #333; }"
".video-container { position: relative; margin: 20px 0; background: #000; min-height: 400px; display: flex; align-items: center; justify-content: center; }"
"#snapshot { max-width: 100%; border-radius: 5px; }"
".placeholder { color: #666; font-size: 18px; }"
".controls { margin: 20px 0; }"
"input[type='text'] { padding: 10px; width: 200px; font-size: 16px; border: 2px solid #ddd; border-radius: 5px; margin-right: 10px; }"
"button { padding: 10px 20px; margin: 5px; font-size: 16px; cursor: pointer; border: none; border-radius: 5px; }"
".btn-capture { background: #2196F3; color: white; }"
".btn-capture:hover { background: #0b7dda; }"
".btn-enroll { background: #4CAF50; color: white; }"
".btn-enroll:hover { background: #45a049; }"
".btn-enroll:disabled { background: #ccc; cursor: not-allowed; }"
".btn-delete { background: #f44336; color: white; }"
".btn-delete:hover { background: #da190b; }"
".btn-reset { background: #ff9800; color: white; }"
".btn-reset:hover { background: #e68900; }"
".status { padding: 10px; margin: 10px 0; border-radius: 5px; }"
".success { background: #d4edda; color: #155724; border: 1px solid #c3e6cb; }"
".error { background: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }"
".face-list { margin: 20px 0; }"
".face-item { padding: 10px; background: #f9f9f9; margin: 5px 0; border-radius: 5px; display: flex; justify-content: space-between; align-items: center; }"
"</style>"
"</head>"
"<body>"
"<div class='container'>"
"<h1>Face Recognition Camera</h1>"
"<p><a href='/recognition' target='_blank' style='color: #2196F3; text-decoration: none;'>Open Live Recognition Stream</a></p>"
"<div class='video-container'>"
"<img id='snapshot' style='display:none'>"
"<div id='placeholder' class='placeholder'>Click 'Capture Photo' to take a snapshot</div>"
"</div>"
"<div class='controls'>"
"<button class='btn-capture' onclick='capturePhoto()'>Capture Photo</button>"
"<br><br>"
"<h3>Enroll Face</h3>"
"<input type='text' id='name-input' placeholder='Enter name...'>"
"<button class='btn-enroll' id='enroll-btn' onclick='enrollFace()' disabled>Enroll Face</button>"
"<br><br>"
"<h3>Database Management</h3>"
"<button class='btn-delete' onclick='deleteAllFaces()'>Delete All Faces</button>"
"<button class='btn-reset' onclick='resetDatabase()'>Reset Database</button>"
"</div>"
"<div id='status'></div>"
"<div class='face-list'>"
"<h3>Enrolled Faces: <span id='face-count'>0</span></h3>"
"<div id='face-items'></div>"
"</div>"
"</div>"
"<script>"
"console.log('JavaScript loaded successfully');"
"let currentImageData = null;"
"function showStatus(msg, success) {"
"  const status = document.getElementById('status');"
"  status.className = 'status ' + (success ? 'success' : 'error');"
"  status.textContent = msg;"
"  setTimeout(() => status.textContent = '', 3000);"
"}"
"function capturePhoto() {"
"  console.log('Capturing photo...');"
"  showStatus('Capturing...', true);"
"  fetch('/capture')"
"    .then(r => r.blob())"
"    .then(blob => {"
"      const url = URL.createObjectURL(blob);"
"      const img = document.getElementById('snapshot');"
"      const placeholder = document.getElementById('placeholder');"
"      img.src = url;"
"      img.style.display = 'block';"
"      placeholder.style.display = 'none';"
"      document.getElementById('enroll-btn').disabled = false;"
"      currentImageData = blob;"
"      showStatus('Photo captured! Enter name and click Enroll', true);"
"    })"
"    .catch(e => {"
"      console.error('Capture error:', e);"
"      showStatus('Capture failed: ' + e.message, false);"
"    });"
"}"
"function enrollFace() {"
"  const name = document.getElementById('name-input').value;"
"  if (!name) { showStatus('Please enter a name', false); return; }"
"  if (!currentImageData) { showStatus('Please capture a photo first', false); return; }"
"  console.log('Enrolling:', name);"
"  showStatus('Enrolling...', true);"
"  const formData = new FormData();"
"  formData.append('image', currentImageData, 'snapshot.jpg');"
"  formData.append('name', name);"
"  fetch('/enroll', {"
"    method: 'POST',"
"    body: formData"
"  })"
"    .then(r => {"
"      console.log('Response status:', r.status);"
"      return r.text();"
"    })"
"    .then(text => {"
"      console.log('Response text:', text);"
"      const d = JSON.parse(text);"
"      if (d.success) {"
"        showStatus('Face enrolled: ' + name, true);"
"        document.getElementById('name-input').value = '';"
"        loadFaces();"
"      } else {"
"        showStatus('Enrollment failed: ' + (d.message || 'Unknown error'), false);"
"      }"
"    })"
"    .catch(e => {"
"      console.error('Error:', e);"
"      showStatus('Error: ' + e.message, false);"
"    });"
"}"
"function deleteAllFaces() {"
"  if (!confirm('Delete all enrolled faces?')) return;"
"  fetch('/delete_all')"
"    .then(r => r.json())"
"    .then(d => {"
"      showStatus('All faces deleted', true);"
"      loadFaces();"
"    })"
"    .catch(e => showStatus('Error: ' + e, false));"
"}"
"function resetDatabase() {"
"  if (!confirm('Reset database and metadata? This will remove all face data and fix corruption issues. This action cannot be undone.')) return;"
"  showStatus('Resetting database...', true);"
"  fetch('/reset_database')"
"    .then(r => r.json())"
"    .then(d => {"
"      if (d.success) {"
"        showStatus('Database reset successfully', true);"
"        loadFaces();"
"      } else {"
"        showStatus('Reset failed: ' + (d.message || 'Unknown error'), false);"
"      }"
"    })"
"    .catch(e => showStatus('Error: ' + e.message, false));"
"}"
"function loadFaces() {"
"  fetch('/faces')"
"    .then(r => r.json())"
"    .then(d => {"
"      document.getElementById('face-count').textContent = d.count;"
"      const items = document.getElementById('face-items');"
"      items.innerHTML = '';"
"      d.faces.forEach(f => {"
"        const div = document.createElement('div');"
"        div.className = 'face-item';"
"        div.innerHTML = '<span>' + f.name + ' (ID: ' + f.id + ')</span>';"
"        items.appendChild(div);"
"      });"
"    });"
"}"
"loadFaces();"
"</script>"
"</body>"
"</html>";

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected from AP, retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// Initialize WiFi
void wifi_init_sta(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        NULL,
                                        NULL);
    esp_event_handler_instance_register(IP_EVENT,
                                        IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler,
                                        NULL,
                                        NULL);

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);
    
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

// Camera initialization
esp_err_t init_camera(void)
{
    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,

        .pin_d7 = CAM_PIN_D9,
        .pin_d6 = CAM_PIN_D8,
        .pin_d5 = CAM_PIN_D7,
        .pin_d4 = CAM_PIN_D6,
        .pin_d3 = CAM_PIN_D5,
        .pin_d2 = CAM_PIN_D4,
        .pin_d1 = CAM_PIN_D3,
        .pin_d0 = CAM_PIN_D2,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA,    // 640x480
        .jpeg_quality = 15,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };

    // Initialize camera
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return err;
    }

    // Get sensor to adjust settings
    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        // Flip image upside down
        s->set_vflip(s, 1);          // Vertical flip
        s->set_hmirror(s, 1);        // Horizontal mirror
        // Adjust settings for better quality
        s->set_brightness(s, 0);     // -2 to 2
        s->set_contrast(s, 0);       // -2 to 2
        s->set_saturation(s, 0);     // -2 to 2
    }

    ESP_LOGI(TAG, "Camera initialized successfully");
    return ESP_OK;
}

// HTTP stream handler
static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char part_buf[64];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }

    ESP_LOGI(TAG, "Stream started");

    while (true) {
        // Try to acquire camera with short timeout to allow enrollment
        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            fb = esp_camera_fb_get();
            xSemaphoreGive(camera_mutex);
        } else {
            // Camera busy, skip this frame
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;

        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }

        esp_camera_fb_return(fb);
        fb = NULL;
        _jpg_buf = NULL;

        if (res != ESP_OK) {
            break;
        }
    }

    ESP_LOGI(TAG, "Stream ended");
    return res;
}

// Root handler - serve HTML page
static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html, strlen(index_html));
}

// Enroll face handler
static esp_err_t enroll_handler(httpd_req_t *req)
{
    char name[MAX_NAME_LENGTH] = {0};
    uint8_t *image_buf = NULL;
    size_t image_len = 0;
    
    // Add CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    ESP_LOGI(TAG, "Enroll handler called");
    
    // Check if it's a POST request with multipart data
    if (req->method == HTTP_POST) {
        char buf[512];
        size_t buf_len;
        int remaining = req->content_len;
        
        ESP_LOGI(TAG, "POST request, content length: %d", remaining);
        
        // Allocate buffer for the image
        image_buf = (uint8_t*)heap_caps_malloc(remaining, MALLOC_CAP_SPIRAM);
        if (!image_buf) {
            ESP_LOGE(TAG, "Failed to allocate image buffer");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Memory allocation failed\"}");
            return ESP_OK;
        }
        
        // Read the entire POST body
        size_t received = 0;
        while (remaining > 0) {
            buf_len = MIN(remaining, sizeof(buf));
            int ret = httpd_req_recv(req, buf, buf_len);
            if (ret <= 0) {
                if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                    continue;
                }
                free(image_buf);
                httpd_resp_send_500(req);
                return ESP_FAIL;
            }
            memcpy(image_buf + received, buf, ret);
            received += ret;
            remaining -= ret;
        }
        
        ESP_LOGI(TAG, "Received %d bytes", received);
        
        // Parse multipart form data
        // Format: boundary + image part + boundary + name part + boundary
        // The image comes first (name="image"), then the name field (name="name")
        
        // Find JPEG image data (starts with FF D8 FF)
        uint8_t *jpeg_start = NULL;
        for (size_t i = 0; i < received - 3; i++) {
            if (image_buf[i] == 0xFF && image_buf[i+1] == 0xD8 && image_buf[i+2] == 0xFF) {
                jpeg_start = &image_buf[i];
                ESP_LOGI(TAG, "Found JPEG start at offset %d", i);
                break;
            }
        }
        
        if (!jpeg_start) {
            ESP_LOGE(TAG, "Failed to find JPEG start marker");
            free(image_buf);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"No JPEG data found\"}");
            return ESP_OK;
        }
        
        // Find JPEG end (FF D9) - search from JPEG start position
        uint8_t *jpeg_end = NULL;
        size_t start_offset = jpeg_start - image_buf;
        for (size_t i = start_offset; i < received - 1; i++) {
            if (image_buf[i] == 0xFF && image_buf[i+1] == 0xD9) {
                jpeg_end = &image_buf[i + 2];  // Include the end marker
                ESP_LOGI(TAG, "Found JPEG end at offset %d", i + 2);
                break;
            }
        }
        
        if (!jpeg_end) {
            ESP_LOGE(TAG, "Failed to find JPEG end marker");
            free(image_buf);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Invalid JPEG data\"}");
            return ESP_OK;
        }
        
        image_len = jpeg_end - jpeg_start;
        ESP_LOGI(TAG, "JPEG size: %d bytes", image_len);
        
        // Now search for the name field AFTER the JPEG data
        // Look for the boundary after the JPEG
        char *name_field = NULL;
        
        // First, try to find "name=\"name\"" pattern
        for (size_t i = (jpeg_end - image_buf); i < received - 20; i++) {
            if (strncmp((char*)&image_buf[i], "name=\"name\"", 11) == 0) {
                name_field = (char*)&image_buf[i];
                ESP_LOGI(TAG, "Found name field at offset %d", i);
                break;
            }
        }
        
        if (name_field) {
            // Find the content after the headers (after \r\n\r\n)
            char *name_start = strstr(name_field, "\r\n\r\n");
            if (name_start) {
                name_start += 4;  // Skip the \r\n\r\n
                // Find the end (either \r\n or --)
                char *name_end = strstr(name_start, "\r\n");
                if (!name_end) {
                    name_end = strstr(name_start, "--");
                }
                if (name_end) {
                    size_t name_len = MIN(name_end - name_start, MAX_NAME_LENGTH - 1);
                    memcpy(name, name_start, name_len);
                    name[name_len] = '\0';
                    ESP_LOGI(TAG, "Extracted name: '%s' (length: %d)", name, name_len);
                } else {
                    ESP_LOGW(TAG, "Could not find name end");
                }
            } else {
                ESP_LOGW(TAG, "Could not find name content start");
            }
        } else {
            ESP_LOGW(TAG, "Could not find name field in multipart data");
            
            // Debug: print the data after JPEG to see what's there
            ESP_LOGI(TAG, "Data after JPEG (100 bytes):");
            size_t debug_start = (jpeg_end - image_buf);
            size_t debug_len = MIN(100, received - debug_start);
            ESP_LOG_BUFFER_HEXDUMP(TAG, jpeg_end, debug_len, ESP_LOG_INFO);
        }
        
        if (strlen(name) == 0) {
            ESP_LOGE(TAG, "Name not extracted");
            free(image_buf);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Name not found in form data\"}");
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Found JPEG: %d bytes, enrolling as: %s", image_len, name);
        
        // Create a fake camera frame buffer from the uploaded image
        camera_fb_t fb;
        fb.buf = jpeg_start;
        fb.len = image_len;
        fb.width = 640;  // Assuming VGA
        fb.height = 480;
        fb.format = PIXFORMAT_JPEG;
        
        // Enroll the face (keep image_buf alive during enrollment)
        int id = face_recognition_enroll(&fb, name);
        
        // Now we can free the buffer
        free(image_buf);
        
        char json[128];
        if (id >= 0) {
            snprintf(json, sizeof(json), "{\"success\":true,\"id\":%d,\"name\":\"%s\"}", id, name);
            ESP_LOGI(TAG, "Enrollment successful, ID: %d", id);
        } else {
            snprintf(json, sizeof(json), "{\"success\":false,\"message\":\"Face detection failed\"}");
            ESP_LOGE(TAG, "Enrollment failed");
        }
        
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, json);
    }
    
    // Fallback: GET request (old behavior for compatibility)
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        ESP_LOGI(TAG, "Query string: %s", query);
        if (httpd_query_key_value(query, "name", name, sizeof(name)) == ESP_OK) {
            ESP_LOGI(TAG, "Enrolling face with name: %s", name);
            
            // Acquire camera with timeout
            if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
                ESP_LOGE(TAG, "Camera busy, timeout");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Camera busy\"}");
                return ESP_OK;
            }
            
            // Capture a frame
            camera_fb_t *fb = esp_camera_fb_get();
            xSemaphoreGive(camera_mutex);
            
            if (!fb) {
                ESP_LOGE(TAG, "Camera capture failed");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Camera capture failed\"}");
                return ESP_OK;
            }
            
            ESP_LOGI(TAG, "Frame captured, size: %d bytes", fb->len);
            
            // Enroll the face
            int id = face_recognition_enroll(fb, name);
            esp_camera_fb_return(fb);
            
            char json[128];
            if (id >= 0) {
                ESP_LOGI(TAG, "Enrollment successful, ID: %d", id);
                snprintf(json, sizeof(json), "{\"success\":true,\"id\":%d,\"name\":\"%s\"}", id, name);
            } else {
                ESP_LOGE(TAG, "Enrollment failed");
                snprintf(json, sizeof(json), "{\"success\":false,\"message\":\"No face detected or enrollment failed\"}");
            }
            
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, json);
        } else {
            ESP_LOGE(TAG, "Failed to parse name parameter");
        }
    } else {
        ESP_LOGE(TAG, "No query string found");
    }
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Missing name parameter\"}");
}

// List enrolled faces handler
static esp_err_t faces_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Faces handler called");
    
    char json[1024];
    int count = face_recognition_get_enrolled_count();
    
    ESP_LOGI(TAG, "Enrolled count: %d", count);
    
    snprintf(json, sizeof(json), "{\"count\":%d,\"faces\":[", count);
    
    for (int i = 0; i < MAX_FACE_ID_COUNT; i++) {
        face_id_t info;
        if (face_recognition_get_info(i, &info) == ESP_OK && info.enrolled) {
            char item[64];
            snprintf(item, sizeof(item), "{\"id\":%d,\"name\":\"%s\"},", info.id, info.name);
            strcat(json, item);
        }
    }
    
    // Remove trailing comma if exists
    int len = strlen(json);
    if (json[len-1] == ',') {
        json[len-1] = '\0';
    }
    
    strcat(json, "]}");
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// Delete all faces handler
static esp_err_t delete_all_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Delete all handler called");
    
    esp_err_t err = face_recognition_delete_all();
    
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"success\":true}");
    } else {
        return httpd_resp_sendstr(req, "{\"success\":false}");
    }
}

// Reset database handler - removes database and metadata files
static esp_err_t reset_database_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Reset database handler called");
    
    esp_err_t err = face_recognition_reset_database();
    
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Database reset successfully\"}");
    } else {
        return httpd_resp_sendstr(req, "{\"success\":false,\"message\":\"Failed to reset database\"}");
    }
}

// Ping test handler
static esp_err_t ping_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Ping handler called");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"pong\"}");
}

// Capture single image handler
static esp_err_t capture_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Capture handler called");
    
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    
    // Acquire camera
    if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Camera busy");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    fb = esp_camera_fb_get();
    xSemaphoreGive(camera_mutex);
    
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Captured image, size: %d bytes", fb->len);
    
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    
    return res;
}

// HTML page for recognition view with overlay
static const char* recognition_html = "<!DOCTYPE html>"
"<html>"
"<head>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<title>Live Face Recognition</title>"
"<style>"
"body { margin: 0; padding: 0; background: #000; font-family: Arial; }"
".container { position: relative; width: 100vw; height: 100vh; display: flex; align-items: center; justify-content: center; overflow: hidden; }"
"#canvas { max-width: 100%; max-height: 100%; }"
".overlay { position: absolute; bottom: 20px; left: 20px; background: rgba(0,0,0,0.8); color: white; padding: 15px 25px; border-radius: 10px; font-size: 28px; font-weight: bold; min-width: 200px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }"
".name { color: #4CAF50; }"
".unknown { color: #ff9800; }"
"</style>"
"</head>"
"<body>"
"<div class='container'>"
"<canvas id='canvas'></canvas>"
"<div class='overlay' id='overlay'>"
"<span id='name' class='unknown'>Scanning...</span>"
"</div>"
"</div>"
"<script>"
"const canvas = document.getElementById('canvas');"
"const ctx = canvas.getContext('2d');"
"const nameEl = document.getElementById('name');"
"const streamUrl = '/recognition_stream';"
"let img = new Image();"
"let currentName = 'Scanning...';"
"async function fetchStream() {"
"  try {"
"    const response = await fetch(streamUrl);"
"    const reader = response.body.getReader();"
"    let buffer = new Uint8Array(0);"
"    const boundary = new TextEncoder().encode('--123456789000000000000987654321');"
"    while (true) {"
"      const {done, value} = await reader.read();"
"      if (done) break;"
"      const temp = new Uint8Array(buffer.length + value.length);"
"      temp.set(buffer);"
"      temp.set(value, buffer.length);"
"      buffer = temp;"
"      let searchStart = 0;"
"      while (true) {"
"        const boundaryPos = findBoundary(buffer, boundary, searchStart);"
"        if (boundaryPos === -1) break;"
"        const nextBoundaryPos = findBoundary(buffer, boundary, boundaryPos + boundary.length);"
"        if (nextBoundaryPos === -1) break;"
"        const chunk = buffer.slice(boundaryPos, nextBoundaryPos);"
"        processChunk(chunk);"
"        buffer = buffer.slice(nextBoundaryPos);"
"        searchStart = 0;"
"      }"
"    }"
"  } catch (e) {"
"    console.error('Stream error:', e);"
"    setTimeout(fetchStream, 1000);"
"  }"
"}"
"function findBoundary(buffer, boundary, start) {"
"  for (let i = start; i <= buffer.length - boundary.length; i++) {"
"    let match = true;"
"    for (let j = 0; j < boundary.length; j++) {"
"      if (buffer[i + j] !== boundary[j]) {"
"        match = false;"
"        break;"
"      }"
"    }"
"    if (match) return i;"
"  }"
"  return -1;"
"}"
"function processChunk(chunk) {"
"  const text = new TextDecoder().decode(chunk);"
"  const headerEnd = text.indexOf('\\r\\n\\r\\n');"
"  if (headerEnd === -1) return;"
"  const headers = text.substring(0, headerEnd);"
"  const nameMatch = headers.match(/X-Face-Name: ([^\\r\\n]+)/);"
"  if (nameMatch) {"
"    const name = nameMatch[1].trim();"
"    if (name !== currentName) {"
"      currentName = name;"
"      if (name !== 'Unknown' && name !== '') {"
"        nameEl.textContent = name;"
"        nameEl.className = 'name';"
"      } else {"
"        nameEl.textContent = 'No face detected';"
"        nameEl.className = 'unknown';"
"      }"
"    }"
"  }"
"  const jpegStart = headerEnd + 4;"
"  const jpegData = chunk.slice(jpegStart);"
"  const blob = new Blob([jpegData], {type: 'image/jpeg'});"
"  const url = URL.createObjectURL(blob);"
"  img.onload = () => {"
"    canvas.width = img.width;"
"    canvas.height = img.height;"
"    ctx.drawImage(img, 0, 0);"
"    URL.revokeObjectURL(url);"
"  };"
"  img.src = url;"
"}"
"fetchStream();"
"</script>"
"</body>"
"</html>";

// Recognition page handler - serves HTML with overlay
static esp_err_t recognition_page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, recognition_html, strlen(recognition_html));
}

// Recognition stream handler - stream with face detection
static esp_err_t recognition_stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char part_buf[128];
    char current_name[MAX_NAME_LENGTH];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }

    ESP_LOGI(TAG, "Recognition stream started");

    while (true) {
        // Try to acquire camera with short timeout
        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            fb = esp_camera_fb_get();
            xSemaphoreGive(camera_mutex);
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        // Get current recognized name (with mutex protection)
        if (xSemaphoreTake(name_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            strcpy(current_name, name);
            xSemaphoreGive(name_mutex);
        } else {
            strcpy(current_name, "Unknown");
        }

        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;

        // Send frame with recognition info in header
        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, sizeof(part_buf), 
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %u\r\n"
                "X-Face-Name: %s\r\n\r\n", 
                _jpg_buf_len, current_name);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }

        esp_camera_fb_return(fb);
        fb = NULL;
        _jpg_buf = NULL;

        if (res != ESP_OK) {
            break;
        }
    }

    ESP_LOGI(TAG, "Recognition stream ended");
    return res;
}

static esp_err_t recognized_name_handler(httpd_req_t *req)
{
    char current_name[MAX_NAME_LENGTH];
    
    // Get current name with mutex protection
    if (xSemaphoreTake(name_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strcpy(current_name, name);
        xSemaphoreGive(name_mutex);
    } else {
        strcpy(current_name, "Unknown");
    }
    
    httpd_resp_set_type(req, "application/json");
    char json[128];
    snprintf(json, sizeof(json), "{\"name\":\"%s\"}", current_name);
    return httpd_resp_sendstr(req, json);
}

// Start HTTP server
void start_camera_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_uri_handlers = 16;  // Increased from 8 to support all our endpoints

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL
    };

    httpd_uri_t enroll_uri_get = {
        .uri = "/enroll",
        .method = HTTP_GET,
        .handler = enroll_handler,
        .user_ctx = NULL
    };
    
    httpd_uri_t enroll_uri_post = {
        .uri = "/enroll",
        .method = HTTP_POST,
        .handler = enroll_handler,
        .user_ctx = NULL
    };

    httpd_uri_t faces_uri = {
        .uri = "/faces",
        .method = HTTP_GET,
        .handler = faces_handler,
        .user_ctx = NULL
    };

    httpd_uri_t delete_all_uri = {
        .uri = "/delete_all",
        .method = HTTP_GET,
        .handler = delete_all_handler,
        .user_ctx = NULL
    };

    httpd_uri_t reset_database_uri = {
        .uri = "/reset_database",
        .method = HTTP_GET,
        .handler = reset_database_handler,
        .user_ctx = NULL
    };

    httpd_uri_t ping_uri = {
        .uri = "/ping",
        .method = HTTP_GET,
        .handler = ping_handler,
        .user_ctx = NULL
    };

    httpd_uri_t capture_uri = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = capture_handler,
        .user_ctx = NULL
    };

    httpd_uri_t recognition_stream_uri = {
        .uri = "/recognition",
        .method = HTTP_GET,
        .handler = recognition_page_handler,
        .user_ctx = NULL
    };

    httpd_uri_t recognized_name_uri = {
        .uri = "/recognized_name",
        .method = HTTP_GET,
        .handler = recognized_name_handler,
        .user_ctx = NULL
    };
    
    httpd_uri_t recognition_stream_data_uri = {
        .uri = "/recognition_stream",
        .method = HTTP_GET,
        .handler = recognition_stream_handler,
        .user_ctx = NULL
    };

    ESP_LOGI(TAG, "Starting web server on port: '%d'", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers...");
        httpd_register_uri_handler(stream_httpd, &index_uri);
        ESP_LOGI(TAG, "Registered: /");
        httpd_register_uri_handler(stream_httpd, &stream_uri);
        ESP_LOGI(TAG, "Registered: /stream");
        httpd_register_uri_handler(stream_httpd, &enroll_uri_get);
        httpd_register_uri_handler(stream_httpd, &enroll_uri_post);
        ESP_LOGI(TAG, "Registered: /enroll (GET and POST)");
        httpd_register_uri_handler(stream_httpd, &faces_uri);
        ESP_LOGI(TAG, "Registered: /faces");
        httpd_register_uri_handler(stream_httpd, &delete_all_uri);
        ESP_LOGI(TAG, "Registered: /delete_all");
        httpd_register_uri_handler(stream_httpd, &reset_database_uri);
        ESP_LOGI(TAG, "Registered: /reset_database");
        httpd_register_uri_handler(stream_httpd, &ping_uri);
        ESP_LOGI(TAG, "Registered: /ping");
        httpd_register_uri_handler(stream_httpd, &capture_uri);
        ESP_LOGI(TAG, "Registered: /capture");
        httpd_register_uri_handler(stream_httpd, &recognized_name_uri);
        ESP_LOGI(TAG, "Registered: /recognized_name");
        
        esp_err_t rec_reg = httpd_register_uri_handler(stream_httpd, &recognition_stream_uri);
        ESP_LOGI(TAG, "Registered: /recognition (result: %d)", rec_reg);
        
        esp_err_t rec_stream_reg = httpd_register_uri_handler(stream_httpd, &recognition_stream_data_uri);
        ESP_LOGI(TAG, "Registered: /recognition_stream (result: %d)", rec_stream_reg);
        
        esp_netif_ip_info_t ip_info;
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            ESP_LOGI(TAG, "Web interface started at http://" IPSTR, IP2STR(&ip_info.ip));
        } else {
            ESP_LOGI(TAG, "Web interface started at http://[YOUR_IP]");
        }
    } else {
        ESP_LOGE(TAG, "Failed to start stream server");
    }
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer;  // Buffer to store response of http request from event handler
    static int output_len;       // Stores number of bytes read
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            // Clean the buffer in case of a new request
            if (output_len == 0 && evt->user_data) {
                // we are just starting to copy the output data into the use
                memset(evt->user_data, 0, MAX_HTTP_OUTPUT_BUFFER);
            }
            /*
             *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
             *  However, event handler can also be used in case chunked encoding is used.
             */
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // If user_data buffer is configured, copy the response into the buffer
                int copy_len = 0;
                if (evt->user_data) {
                    // The last byte in evt->user_data is kept for the NULL character in case of out-of-bound access.
                    copy_len = MIN(evt->data_len, (MAX_HTTP_OUTPUT_BUFFER - output_len));
                    if (copy_len) {
                        memcpy((char *)evt->user_data + output_len, evt->data, copy_len);
                    }
                } else {
                    int content_len = esp_http_client_get_content_length(evt->client);
                    if (output_buffer == NULL) {
                        // We initialize output_buffer with 0 because it is used by strlen() and similar functions therefore should be null terminated.
                        output_buffer = (char *) calloc(content_len + 1, sizeof(char));
                        output_len = 0;
                        if (output_buffer == NULL) {
                            ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                            return ESP_FAIL;
                        }
                    }
                    copy_len = MIN(evt->data_len, (content_len - output_len));
                    if (copy_len) {
                        memcpy(output_buffer + output_len, evt->data, copy_len);
                    }
                }
                output_len += copy_len;
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            if (output_buffer != NULL) {
#if CONFIG_EXAMPLE_ENABLE_RESPONSE_BUFFER_DUMP
                ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
#endif
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
        case HTTP_EVENT_DISCONNECTED:
            {
                ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
                int mbedtls_err = 0;
                esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
                if (err != 0) {
                    ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                    ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
                }
                if (output_buffer != NULL) {
                    free(output_buffer);
                    output_buffer = NULL;
                }
                output_len = 0;
            }
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
            // For Discord webhook, follow redirects automatically
            esp_http_client_set_redirection(evt->client);
            break;
        default:
            break;
    }
    return ESP_OK;
}

void init_neopixel(void)
{
    /* LED strip initialization with the GPIO and pixels number */
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = WS2812_PIN;
    strip_config.max_leds = 1;
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB;
    strip_config.flags.invert_out = false;
    
    /* RMT backend configuration */
    led_strip_rmt_config_t rmt_config = {};
    rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_config.resolution_hz = 10 * 1000 * 1000; // 10MHz
    rmt_config.flags.with_dma = false;
    
    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize NeoPixel LED on GPIO %d: %s (may conflict with JTAG)", 
                 WS2812_PIN, esp_err_to_name(err));
        ESP_LOGW(TAG, "NeoPixel LED will be disabled. This is normal if using JTAG debugging.");
        led_strip = NULL;
        return;
    }
    
    ESP_LOGI(TAG, "NeoPixel LED initialized on GPIO %d", WS2812_PIN);
}

void set_neopixel_color(int r, int g, int b)
{
    if (led_strip == NULL) {
        ESP_LOGW(TAG, "LED strip not initialized");
        return;
    }
    
    esp_err_t err = led_strip_set_pixel(led_strip, 0, r, g, b);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set LED pixel: %s", esp_err_to_name(err));
        return;
    }
    
    err = led_strip_refresh(led_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to refresh LED: %s", esp_err_to_name(err));
    }
}

// Task to send Discord message (needs larger stack than main task)
void discord_task(void *param) {
    char *message = (char *)param;
    char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER + 1] = {0};
    char json_data[512];
    snprintf(json_data, sizeof(json_data), "{\"content\":\"%s\"}", message);
    
    esp_http_client_config_t config = {};
    // Change config.url with your Discord webhook
    config.url = "";
    config.event_handler = _http_event_handler;
    config.user_data = local_response_buffer;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 5000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(message);
        vTaskDelete(NULL);
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_data, strlen(json_data));
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Discord message sent successfully");
    } else {
        ESP_LOGE(TAG, "Failed to send Discord message: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    free(message);
    vTaskDelete(NULL);
}

bool sendDiscordMessage(const char* message) {
    // Allocate message string for task
    char *msg_copy = (char *)malloc(strlen(message) + 1);
    if (msg_copy == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for Discord message");
        return false;
    }
    strcpy(msg_copy, message);
    
    // Create task with 8KB stack (enough for HTTP client)
    BaseType_t result = xTaskCreate(discord_task, "discord", 8192, msg_copy, 5, NULL);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Discord task");
        free(msg_copy);
        return false;
    }
    return true;
}

// Background task to continuously perform face recognition
void face_recognition_task(void *param)
{
    ESP_LOGI(TAG, "Face recognition background task started");
    char local_name[MAX_NAME_LENGTH];
    
    while (true) {
        // Wait for the recognition interval
        vTaskDelay(pdMS_TO_TICKS(RECOGNITION_INTERVAL_MS));
        
        // Try to acquire camera with short timeout
        if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;  // Camera busy, skip this cycle
        }
        
        camera_fb_t *fb = esp_camera_fb_get();
        
        if (fb) {
            // Perform face recognition
            int result = face_recognition_recognize(fb, local_name);
            
            // Update global name with mutex protection
            if (xSemaphoreTake(name_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (result >= 0) {
                    // Face recognized
                    strcpy(name, local_name);
                    
                    // Check if this is a new recognition or enough time has passed
                    TickType_t current_time = xTaskGetTickCount();
                    bool should_send = false;
                    
                    if (strcmp(name, last_sent_name) != 0) {
                        // Different person detected
                        should_send = true;
                        ESP_LOGI(TAG, "New person recognized: %s", name);
                    } else if ((current_time - last_recognition_time) > pdMS_TO_TICKS(RECOGNITION_COOLDOWN_MS)) {
                        // Same person but cooldown period has passed
                        should_send = true;
                        ESP_LOGI(TAG, "Re-sending recognition for: %s (cooldown expired)", name);
                    }
                    
                    if (should_send) {
                        // Send Discord notification
                        char discord_msg[128];
                        snprintf(discord_msg, sizeof(discord_msg), "🎥 Spotted: %s", name);
                        sendDiscordMessage(discord_msg);
                        
                        // Update tracking variables
                        strcpy(last_sent_name, name);
                        last_recognition_time = current_time;
                        
                        // Set LED to green for recognized face
                        set_neopixel_color(0, 255, 0);
                        vTaskDelay(pdMS_TO_TICKS(500));
                        set_neopixel_color(0, 0, 255);  // Back to blue
                    }
                } else {
                    // No face or unknown face
                    strcpy(name, "Unknown");
                }
                xSemaphoreGive(name_mutex);
            }
            
            esp_camera_fb_return(fb);
        }
        
        xSemaphoreGive(camera_mutex);
    }
}

extern "C" void app_main(void)
{   
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Create camera mutex
    camera_mutex = xSemaphoreCreateMutex();
    if (camera_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create camera mutex");
        return;
    }

    // Create name mutex
    name_mutex = xSemaphoreCreateMutex();
    if (name_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create name mutex");
        return;
    }

    ESP_LOGI(TAG, "Initializing WiFi...");
    wifi_init_sta();

    // Wait for WiFi connection
    vTaskDelay(5000 / portTICK_PERIOD_MS);

    ESP_LOGI(TAG, "Initializing camera...");
    if (init_camera() != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed");
        return;
    }

    ESP_LOGI(TAG, "Initializing face recognition...");
    face_recognition_init();

    ESP_LOGI(TAG, "Initializing NeoPixel LED...");
    init_neopixel();

    ESP_LOGI(TAG, "Starting camera server...");
    start_camera_server();

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "Setup complete. Access web interface at http://" IPSTR, IP2STR(&ip_info.ip));
        char discord_msg[256];
        snprintf(discord_msg, sizeof(discord_msg), "ESP32-S3-CAM started. http://" IPSTR, IP2STR(&ip_info.ip));
        sendDiscordMessage(discord_msg);
    } else {
        ESP_LOGI(TAG, "Setup complete. Access web interface at http://[YOUR_IP]");
        sendDiscordMessage("ESP32-S3-CAM started. IP Address: Unknown");
    }
    
    // Set Neopixel to blue to indicate ready
    set_neopixel_color(0, 0, 255);
    
    // Start background face recognition task
    ESP_LOGI(TAG, "Starting background face recognition task...");
    xTaskCreate(face_recognition_task, "face_recog", 8192, NULL, 5, NULL);
}