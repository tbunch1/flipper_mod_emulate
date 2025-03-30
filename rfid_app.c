#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_rfid.h>

#include <lib/lfrfid/lfrfid_dict_file.h>
#include <lib/lfrfid/lfrfid_worker.h>


// #include <lfrfid/protocols/lfrfid_protocols.h>  <- this works but not the one below
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
    RfidAppStateMenu,
    RfidAppStateInputOffset,
    RfidAppStateInputData,
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
    uint8_t current_offset;  // Current offset value
    uint8_t menu_selection;  // Current menu selection
    uint8_t input_position; // Position in data input
    char input_buffer[17];  // Buffer for hex input (8 bytes = 16 chars + null)
} RfidApp;

static void app_draw_callback(Canvas* canvas, void* ctx) {
    RfidApp* app = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "RFID Tool");
    
    canvas_set_font(canvas, FontSecondary);
    switch(app->state) {
        case RfidAppStateIdle:
            canvas_draw_str(canvas, 2, 24, "OK: Read, Up: Menu");
            canvas_draw_str(canvas, 2, 36, "Back: Exit");
            if(app->tag_found) {
                canvas_draw_str(canvas, 2, 48, "Tag found!");
            }
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
        case RfidAppStateMenu:
            canvas_draw_str(canvas, 2, 24, "Menu:");
            canvas_draw_str(canvas, 2, 36, app->menu_selection == 0 ? "> Set Offset" : "  Set Offset");
            canvas_draw_str(canvas, 2, 48, app->menu_selection == 1 ? "> Input Data" : "  Input Data");
            canvas_draw_str(canvas, 2, 60, app->menu_selection == 2 ? "> Write Tag" : "  Write Tag");
            break;
        case RfidAppStateInputOffset:
            canvas_draw_str(canvas, 2, 24, "Enter Offset (0-255):");
            char offset_str[8];
            snprintf(offset_str, sizeof(offset_str), "%d", app->current_offset);
            canvas_draw_str(canvas, 2, 36, offset_str);
            break;
        case RfidAppStateInputData:
            canvas_draw_str(canvas, 2, 24, "Enter Data (16 hex):");
            canvas_draw_str(canvas, 2, 36, app->input_buffer);
            if(app->input_position < 16) {
                canvas_draw_str(canvas, 2 + (app->input_position * 6), 36, "_");
            }
            canvas_draw_str(canvas, 2, 48, "Up:0 Down:1 Left:2 Right:3");
            canvas_draw_str(canvas, 2, 60, "OK:Confirm Back:Delete");
            break;
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
    uint16_t carry = app->current_offset;
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

static void handle_menu_input(RfidApp* app, InputEvent* event) {
    if(event->type == InputTypeShort) {
        switch(event->key) {
            case InputKeyUp:
                if(app->menu_selection > 0) app->menu_selection--;
                break;
            case InputKeyDown:
                if(app->menu_selection < 2) app->menu_selection++;
                break;
            case InputKeyOk:
                switch(app->menu_selection) {
                    case 0:
                        app->state = RfidAppStateInputOffset;
                        app->input_position = 0;
                        break;
                    case 1:
                        app->state = RfidAppStateInputData;
                        app->input_position = 0;
                        memset(app->input_buffer, 0, sizeof(app->input_buffer));
                        break;
                    case 2:
                        app->state = RfidAppStateIdle;
                        rfid_write_tag(app);
                        break;
                }
                break;
            case InputKeyBack:
                app->state = RfidAppStateIdle;
                break;
            case InputKeyLeft:
            case InputKeyRight:
            case InputKeyMAX:
                break;
        }
    }
}

static void handle_offset_input(RfidApp* app, InputEvent* event) {
    if(event->type == InputTypeShort) {
        switch(event->key) {
            case InputKeyUp:
                if(app->current_offset < 255) {
                    app->current_offset++;
                } else {
                    app->current_offset = 0;  // Wrap around to 0
                }
                break;
            case InputKeyDown:
                if(app->current_offset > 0) {
                    app->current_offset--;
                } else {
                    app->current_offset = 255;  // Wrap around to 255
                }
                break;
            case InputKeyOk:
                app->state = RfidAppStateMenu;
                break;
            case InputKeyBack:
                app->state = RfidAppStateMenu;
                break;
            case InputKeyLeft:
            case InputKeyRight:
            case InputKeyMAX:
                break;
        }
    }
}

static void handle_data_input(RfidApp* app, InputEvent* event) {
    if(event->type == InputTypeShort) {
        switch(event->key) {
            case InputKeyOk:
                if(app->input_position < 16) {
                    app->input_buffer[app->input_position] = '0';
                    app->input_position++;
                } else {
                    // Validate hex string
                    bool valid = true;
                    for(size_t i = 0; i < 16; i++) {
                        if(!isxdigit((unsigned char)app->input_buffer[i])) {
                            valid = false;
                            break;
                        }
                    }
                    if(valid) {
                        // Convert hex string to bytes
                        for(int i = 0; i < 8; i++) {
                            char hex[3] = {app->input_buffer[i*2], app->input_buffer[i*2+1], 0};
                            app->tag_data[i] = strtol(hex, NULL, 16);
                        }
                        app->state = RfidAppStateMenu;
                    } else {
                        error_beep();
                    }
                }
                break;
            case InputKeyBack:
                if(app->input_position > 0) {
                    app->input_position--;
                    app->input_buffer[app->input_position] = 0;
                } else {
                    app->state = RfidAppStateMenu;
                }
                break;
            case InputKeyUp:
            case InputKeyDown:
            case InputKeyLeft:
            case InputKeyRight:
                if(app->input_position < 16) {
                    char c = 0;
                    switch(event->key) {
                        case InputKeyUp: c = '0'; break;
                        case InputKeyDown: c = '1'; break;
                        case InputKeyLeft: c = '2'; break;
                        case InputKeyRight: c = '3'; break;
                        case InputKeyOk: c = '4'; break;
                        case InputKeyMAX: c = '6'; break;
                        case InputKeyBack:
                            app->state = RfidAppStateMenu;
                            break;
                    }
                    if(c) {
                        app->input_buffer[app->input_position] = c;
                        app->input_position++;
                    }
                }
                break;
            case InputKeyMAX:
                break;
        }
    }
}

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
                switch(app->state) {
                    case RfidAppStateIdle:
                        switch(event.key) {
                            case InputKeyOk:
                                rfid_read_tag(app);
                                break;
                            case InputKeyUp:
                                app->state = RfidAppStateMenu;
                                break;
                            case InputKeyBack:
                                running = false;
                                break;
                            default:
                                break;
                        }
                        break;
                    case RfidAppStateMenu:
                        handle_menu_input(app, &event);
                        break;
                    case RfidAppStateInputOffset:
                        handle_offset_input(app, &event);
                        break;
                    case RfidAppStateInputData:
                        handle_data_input(app, &event);
                        break;
                    case RfidAppStateEmulating:
                        if(event.key == InputKeyBack) {
                            lfrfid_worker_stop(app->worker);
                            app->state = RfidAppStateIdle;
                        }
                        break;
                    default:
                        break;
                }
            }
        }
        view_port_update(app->view_port);
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