#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_rfid.h>

#include <lib/lfrfid/lfrfid_dict_file.h>
#include <lib/lfrfid/lfrfid_worker.h>


// #include <lfrfid/protocols/lfrfid_protocols.h> 
// #include <lib/lfrfid/protocols/protocol_hid_generic.h>

#include <gui/gui.h>
#include <input/input.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#include <notification/notification.h>
#include <notification/notification_messages.h>

#include "rfid_app_icons.h"

#define TAG "RFID_APP"
#define DATA_OFFSET 1  // Constant offset for modifying tag data

typedef enum {
    RfidAppStateIdle,
    RfidAppStateReading,
    RfidAppStateEmulating,
    RfidAppStateWriting,
} RfidAppState;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    RfidAppState state;
    uint8_t tag_data[8];  // HID data is 8 bytes
    bool tag_found;
    LFRFIDWorker* worker;
    ProtocolDict* protocols;
    FuriString* status_text;
} RfidApp;

static void app_draw_callback(Canvas* canvas, void* ctx) {
    RfidApp* app = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "RFID Tool");
    
    canvas_set_font(canvas, FontSecondary);
    switch(app->state) {
        case RfidAppStateIdle:
            canvas_draw_str(canvas, 2, 24, "OK: Read, Up: Write");
            canvas_draw_str(canvas, 2, 36, "Back: Exit");
            break;
        case RfidAppStateReading:
            canvas_draw_str(canvas, 2, 24, "Reading...");
            canvas_draw_str(canvas, 2, 36, "Place tag near");
            if(app->status_text) {
                canvas_draw_str(canvas, 2, 48, furi_string_get_cstr(app->status_text));
            }
            break;
        case RfidAppStateEmulating:
            canvas_draw_str(canvas, 2, 24, "Emulating tag");
            canvas_draw_str(canvas, 2, 36, "Press Back to stop");
            break;
        case RfidAppStateWriting:
            canvas_draw_str(canvas, 2, 24, "Writing...");
            canvas_draw_str(canvas, 2, 36, "Place tag near");
            break;
    }

    if(app->tag_found) {
        canvas_draw_str(canvas, 2, 60, "Tag found!");
    }
}

static void app_input_callback(InputEvent* input_event, void* ctx) {
    RfidApp* app = ctx;
    furi_message_queue_put(app->event_queue, input_event, FuriWaitForever);
}

static void beep() {
    NotificationApp* notification = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notification, &sequence_success);
    furi_record_close(RECORD_NOTIFICATION);
}

static void error_beep() {
    NotificationApp* notification = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notification, &sequence_error);
    furi_record_close(RECORD_NOTIFICATION);
}

static void rfid_read_callback(LFRFIDWorkerReadResult result, ProtocolId protocol, void* context) {
    RfidApp* app = context;
    if(result == LFRFIDWorkerReadDone) {
        // Get the protocol data
        size_t data_size = protocol_dict_get_data_size(app->protocols, protocol);
        protocol_dict_get_data(app->protocols, protocol, app->tag_data, data_size);
        
        app->tag_found = true;
        app->state = RfidAppStateIdle;  // Return to idle state after successful read
        furi_string_set(app->status_text, "Tag read successfully!");
        beep();
    } else if(result == LFRFIDWorkerReadSenseCardStart) {
        furi_string_set(app->status_text, "Card detected, reading...");
    } else if(result == LFRFIDWorkerReadSenseCardEnd) {
        app->state = RfidAppStateIdle;  // Return to idle state if card is removed
        furi_string_set(app->status_text, "Card removed");
    }
}

static void rfid_write_callback(LFRFIDWorkerWriteResult result, void* context) {
    RfidApp* app = context;
    if(result == LFRFIDWorkerWriteOK) {
        app->state = RfidAppStateIdle;  // Return to idle state after successful write
        beep();
    } else {
        app->state = RfidAppStateIdle;  // Return to idle state after failed write
        error_beep();
    }
}

static void rfid_read_tag(RfidApp* app) {
    app->state = RfidAppStateReading;
    app->tag_found = false;
    
    // Update status
    furi_string_set(app->status_text, "Starting field detection...");
    
    // Start reading
    lfrfid_worker_read_start(app->worker, LFRFIDWorkerReadTypeAuto, rfid_read_callback, app);
}

static void rfid_write_tag(RfidApp* app) {
    if(!app->tag_found) {
        error_beep();
        return;
    }

    app->state = RfidAppStateWriting;
    
    // Apply offset to the tag data
    uint8_t modified_data[8];
    memcpy(modified_data, app->tag_data, 8);
    
    // Apply offset with overflow propagation through all bytes
    uint16_t carry = DATA_OFFSET;
    for(int i = 0; i < 8; i++) {
        uint16_t new_value = modified_data[i] + carry;
        modified_data[i] = new_value & 0xFF;  // Keep only the lower 8 bits
        carry = (new_value > 0xFF) ? 1 : 0;   // Set carry for next byte if overflow
    }
    
    // Set the modified data in the protocol dictionary
    protocol_dict_set_data(app->protocols, LFRFIDProtocolHidGeneric, modified_data, 8);
    
    // Start writing
    lfrfid_worker_write_start(app->worker, LFRFIDProtocolHidGeneric, rfid_write_callback, app);
}

// static void rfid_emulate_tag(RfidApp* app) {
//     app->state = RfidAppStateEmulating;
    
//     // Initialize RFID hardware
//     furi_hal_rfid_init();
    
//     // Configure for emulation
//     furi_hal_rfid_tim_read_start(125000, 0.5);
    
//     // Emulate the tag
//     // Note: This is a simplified version. In a real application,
//     // you would need to implement the specific protocol emulation logic
    
//     app->state = RfidAppStateIdle;
// }

int32_t rfid_app_main(void* p) {
    UNUSED(p);
    
    RfidApp* app = malloc(sizeof(RfidApp));
    app->state = RfidAppStateIdle;
    app->tag_found = false;
    app->status_text = furi_string_alloc();
    
    // Initialize protocols and worker
    app->protocols = protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax);
    app->worker = lfrfid_worker_alloc(app->protocols);
    lfrfid_worker_start_thread(app->worker);
    
    // Configure view port
    app->view_port = view_port_alloc();
    app->gui = furi_record_open(RECORD_GUI);
    view_port_draw_callback_set(app->view_port, app_draw_callback, app);
    view_port_input_callback_set(app->view_port, app_input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    
    // Configure event queue
    app->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    
    // Main event loop
    InputEvent event;
    bool running = true;
    
    while(running) {
        if(furi_message_queue_get(app->event_queue, &event, 100) == FuriStatusOk) {
            if(event.type == InputTypeShort) {
                switch(event.key) {
                    case InputKeyOk:
                        if(app->state == RfidAppStateIdle) {
                            rfid_read_tag(app);
                        }
                        break;
                    case InputKeyUp:
                        if(app->state == RfidAppStateIdle) {
                            rfid_write_tag(app);
                        }
                        break;
                    case InputKeyBack:
                        if(app->state == RfidAppStateEmulating) {
                            lfrfid_worker_stop(app->worker);
                            app->state = RfidAppStateIdle;
                        } else {
                            running = false;
                        }
                        break;
                    default:
                        break;
                }
            }
        }
    }
    
    // Cleanup
    lfrfid_worker_stop(app->worker);
    lfrfid_worker_stop_thread(app->worker);
    lfrfid_worker_free(app->worker);
    protocol_dict_free(app->protocols);
    view_port_enabled_set(app->view_port, false);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->event_queue);
    furi_string_free(app->status_text);
    free(app);
    
    return 0;
} 