#include "face_recognition.h"
#include "esp_log.h"
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "dl_image_jpeg.hpp"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"

static const char *TAG = "face_recognition";

// High-level wrapper classes - much simpler!
static human_face_detect::MSRMNP *face_detector = nullptr;
static HumanFaceRecognizer *face_recognizer = nullptr;

static face_id_t face_database[MAX_FACE_ID_COUNT];
static int32_t enrolled_count = 0;
static const char *db_path = "/spiflash/face.db";
static const char *nvs_namespace = "face_db";
static const char *metadata_path = "/spiflash/face_meta.dat";

// Save face metadata to file
static esp_err_t save_face_metadata(void)
{
    FILE *f = fopen(metadata_path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open metadata file for writing");
        return ESP_FAIL;
    }
    
    // Write enrolled count
    fwrite(&enrolled_count, sizeof(enrolled_count), 1, f);
    
    // Write face database
    fwrite(face_database, sizeof(face_database), 1, f);
    
    fclose(f);
    ESP_LOGI(TAG, "Face metadata saved (%d faces)", enrolled_count);
    return ESP_OK;
}

// Load face metadata from file
static esp_err_t load_face_metadata(void)
{
    FILE *f = fopen(metadata_path, "rb");
    if (f == NULL) {
        ESP_LOGI(TAG, "No metadata file found, starting fresh");
        return ESP_ERR_NOT_FOUND;
    }
    
    // Read enrolled count
    size_t count_read = fread(&enrolled_count, sizeof(enrolled_count), 1, f);
    if (count_read != 1) {
        ESP_LOGE(TAG, "Failed to read enrolled count");
        fclose(f);
        return ESP_FAIL;
    }
    
    // Read face database
    size_t db_read = fread(face_database, sizeof(face_database), 1, f);
    if (db_read != 1) {
        ESP_LOGE(TAG, "Failed to read face database");
        fclose(f);
        return ESP_FAIL;
    }
    
    fclose(f);
    ESP_LOGI(TAG, "Face metadata loaded (%d faces)", enrolled_count);
    
    // Log loaded faces
    for (int i = 0; i < MAX_FACE_ID_COUNT; i++) {
        if (face_database[i].enrolled) {
            ESP_LOGI(TAG, "  - ID %d: %s", face_database[i].id, face_database[i].name);
        }
    }
    
    return ESP_OK;
}

void face_recognition_init(void)
{
    ESP_LOGI(TAG, "Initializing face recognition");
    
    // Mount SPIFFS partition for face database
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiflash",
        .partition_label = "fr",
        .max_files = 5,
        .format_if_mount_failed = true
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }
    
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS partition: total: %d, used: %d", total, used);
    }
    
    // Create face detector with MSR+MNP models (include .espdl extension)
    face_detector = new human_face_detect::MSRMNP(
        "human_face_detect_msr_s8_v1.espdl",  // MSR model for detection
        0.3f,  // MSR score threshold
        0.3f,  // MSR NMS threshold  
        "human_face_detect_mnp_s8_v1.espdl",  // MNP model for landmarks
        0.3f,  // MNP score threshold
        0.3f   // MNP NMS threshold
    );
    
    // Check if face database file exists
    struct stat st;
    bool db_exists = (stat(db_path, &st) == 0);
    
    if (db_exists) {
        ESP_LOGI(TAG, "Found existing face database (size: %ld bytes)", st.st_size);
        
        // Create face recognizer and load existing database
        face_recognizer = new HumanFaceRecognizer(
            db_path,
            HumanFaceFeat::MFN_S8_V1,  // Recognition model
            false  // Don't use lazy loading - load the database immediately
        );
        
        // Load face metadata (names)
        if (load_face_metadata() == ESP_OK) {
            // Verify enrolled count matches what's in the database
            int db_count = face_recognizer->get_num_feats();
            if (db_count != enrolled_count) {
                ESP_LOGW(TAG, "Metadata count (%d) doesn't match database count (%d), using database count", 
                         enrolled_count, db_count);
                enrolled_count = db_count;
            }
            ESP_LOGI(TAG, "Loaded %d enrolled faces from persistent storage", enrolled_count);
        } else {
            ESP_LOGW(TAG, "Failed to load metadata, starting fresh");
            enrolled_count = 0;
            memset(face_database, 0, sizeof(face_database));
        }
    } else {
        ESP_LOGI(TAG, "No existing database found, creating new one");
        
        // Create face recognizer with fresh database
        face_recognizer = new HumanFaceRecognizer(
            db_path,
            HumanFaceFeat::MFN_S8_V1,  // Recognition model
            true  // Use lazy loading - database will be created on first use
        );
        
        enrolled_count = 0;
        memset(face_database, 0, sizeof(face_database));
    }
    
    ESP_LOGI(TAG, "Face recognition initialized (%d enrolled faces)", enrolled_count);
}

int face_recognition_recognize(camera_fb_t *fb, char *name_out)
{
    if (!fb || !name_out || !face_detector || !face_recognizer) {
        ESP_LOGE(TAG, "Invalid parameters or not initialized");
        return -1;
    }

    // Convert camera frame buffer to dl::image::img_t
    dl::image::jpeg_img_t jpeg_img = {
        .data = (void *)fb->buf,
        .data_len = (size_t)fb->len
    };
    
    // Decode JPEG to RGB888
    auto img = dl::image::sw_decode_jpeg(jpeg_img, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
    if (!img.data) {
        ESP_LOGE(TAG, "Failed to decode JPEG");
        return -1;
    }
    
    // Detect faces
    auto detect_results = face_detector->run(img);
    
    if (detect_results.size() > 0) {
        ESP_LOGI(TAG, "Detected %zu face(s)", detect_results.size());
        
        // Recognize faces
        auto results = face_recognizer->recognize(img, detect_results);
        
        if (results.size() > 0) {
            // Get the best match
            auto &best = results.front();
            
            // Find name from database
            for (int i = 0; i < enrolled_count; i++) {
                if (face_database[i].id == best.id) {
                    strcpy(name_out, face_database[i].name);
                    ESP_LOGI(TAG, "Recognized: %s (ID: %d, Similarity: %.3f)", 
                             name_out, best.id, best.similarity);
                    heap_caps_free(img.data);
                    return best.id;
                }
            }
        } else {
            ESP_LOGD(TAG, "Face detected but not recognized");
        }
    } else {
        ESP_LOGD(TAG, "No face detected");
    }

    heap_caps_free(img.data);
    return -1;
}

int face_recognition_enroll(camera_fb_t *fb, const char *name)
{
    if (!fb || !name || !face_detector || !face_recognizer) {
        ESP_LOGE(TAG, "Cannot enroll: invalid params or not initialized");
        return -1;
    }

    // Convert camera frame buffer to dl::image::img_t
    dl::image::jpeg_img_t jpeg_img = {
        .data = (void *)fb->buf,
        .data_len = (size_t)fb->len
    };
    
    // Decode JPEG to RGB888
    auto img = dl::image::sw_decode_jpeg(jpeg_img, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
    if (!img.data) {
        ESP_LOGE(TAG, "Failed to decode JPEG");
        return -1;
    }

    ESP_LOGI(TAG, "Converted to RGB888 for enrollment: %dx%d", img.width, img.height);

    // Detect faces
    ESP_LOGI(TAG, "Running face detection...");
    auto detect_results = face_detector->run(img);
    
    ESP_LOGI(TAG, "Detection complete, found %zu face(s)", detect_results.size());
    
    if (detect_results.size() == 0) {
        ESP_LOGE(TAG, "No face detected in image. Try better lighting or a clearer face view.");
        heap_caps_free(img.data);
        return -1;
    }
    
    if (detect_results.size() > 1) {
        ESP_LOGW(TAG, "Multiple faces detected (%zu), using first one", detect_results.size());
    }

    // Get the first detected face
    auto &face = detect_results.front();
    ESP_LOGI(TAG, "Face detected - Score: %.3f, Box: [%d,%d,%d,%d]", 
             face.score, face.box[0], face.box[1], face.box[2], face.box[3]);
    
    // Debug: Check if keypoints are present
    ESP_LOGI(TAG, "Face has %zu keypoints", face.keypoint.size());

    // Enroll the face
    esp_err_t ret = face_recognizer->enroll(img, detect_results);
    
    heap_caps_free(img.data);
    
    if (ret == ESP_OK) {
        // Get the new ID (last enrolled)
        enrolled_count = face_recognizer->get_num_feats();
        int id = enrolled_count - 1;
        
        // Update our local database
        if (id < MAX_FACE_ID_COUNT) {
            face_database[id].id = id;
            face_database[id].enrolled = true;
            strncpy(face_database[id].name, name, MAX_NAME_LENGTH - 1);
            face_database[id].name[MAX_NAME_LENGTH - 1] = '\0';
            face_database[id].template_count = 1;
            
            // Save metadata to persistent storage
            save_face_metadata();
            
            ESP_LOGI(TAG, "Successfully enrolled '%s' with ID %d (Total enrolled: %d)", 
                     name, id, enrolled_count);
            return id;
        }
    }
    
    ESP_LOGE(TAG, "Enrollment failed");
    return -1;
}

int face_recognition_delete_all(void)
{
    if (!face_recognizer) {
        return ESP_FAIL;
    }
    
    esp_err_t ret = face_recognizer->clear_all_feats();
    if (ret == ESP_OK) {
        enrolled_count = 0;
        memset(face_database, 0, sizeof(face_database));
        
        // Delete metadata file
        unlink(metadata_path);
        
        ESP_LOGI(TAG, "Deleted all faces");
    }
    return ret;
}

int face_recognition_get_enrolled_count(void)
{
    return enrolled_count;
}

esp_err_t face_recognition_get_info(int id, face_id_t *info)
{
    if (!info || id < 0 || id >= MAX_FACE_ID_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!face_database[id].enrolled) {
        return ESP_ERR_NOT_FOUND;
    }
    
    memcpy(info, &face_database[id], sizeof(face_id_t));
    return ESP_OK;
}

esp_err_t face_recognition_delete(int id)
{
    if (!face_recognizer || id < 0 || id >= MAX_FACE_ID_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!face_database[id].enrolled) {
        return ESP_ERR_NOT_FOUND;
    }
    
    // Delete from recognizer
    esp_err_t ret = face_recognizer->delete_feat(id);
    if (ret == ESP_OK) {
        // Update local database
        face_database[id].enrolled = false;
        memset(face_database[id].name, 0, MAX_NAME_LENGTH);
        face_database[id].template_count = 0;
        enrolled_count--;
        
        // Save updated metadata
        save_face_metadata();
        
        ESP_LOGI(TAG, "Deleted face ID %d", id);
    }
    
    return ret;
}

esp_err_t face_recognition_reset_database(void)
{
    ESP_LOGW(TAG, "Resetting face database and metadata...");
    
    // Delete the recognizer object
    if (face_recognizer) {
        delete face_recognizer;
        face_recognizer = nullptr;
    }
    
    // Delete database and metadata files
    unlink(db_path);
    unlink(metadata_path);
    
    // Clear in-memory database
    enrolled_count = 0;
    memset(face_database, 0, sizeof(face_database));
    
    // Small delay to ensure filesystem operations complete
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Recreate the recognizer with fresh database
    face_recognizer = new HumanFaceRecognizer(
        db_path,
        HumanFaceFeat::MFN_S8_V1,
        true  // Use lazy loading
    );
    
    ESP_LOGI(TAG, "Database and metadata reset complete");
    return ESP_OK;
}

const face_id_t* face_recognition_get_enrolled_ids(void)
{
    return face_database;
}
