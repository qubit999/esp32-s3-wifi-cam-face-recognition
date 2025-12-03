#ifndef FACE_RECOGNITION_H
#define FACE_RECOGNITION_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "esp_camera.h"

#define MAX_FACE_ID_COUNT 10
#define MAX_NAME_LENGTH 32
#define MAX_FACE_TEMPLATES 5  // Max templates per person

typedef struct {
    char name[MAX_NAME_LENGTH];
    int id;
    bool enrolled;
    int template_count;  // Number of face templates for this person
} face_id_t;

// Initialize face recognition system
void face_recognition_init(void);

// Detect and recognize faces in the frame buffer
// Returns the ID of recognized face, or -1 if no face or unknown face
int face_recognition_recognize(camera_fb_t *fb, char *name_out);

// Enroll a new face with the given name
// Returns face ID on success, -1 on failure
int face_recognition_enroll(camera_fb_t *fb, const char *name);

// Delete a face by ID
esp_err_t face_recognition_delete(int id);

// Delete all enrolled faces
esp_err_t face_recognition_delete_all(void);

// Reset database and metadata (removes files and reinitializes)
esp_err_t face_recognition_reset_database(void);

// Get count of enrolled faces
int face_recognition_get_enrolled_count(void);

// Get face info by ID
esp_err_t face_recognition_get_info(int id, face_id_t *info);

#ifdef __cplusplus
}
#endif

#endif // FACE_RECOGNITION_H
