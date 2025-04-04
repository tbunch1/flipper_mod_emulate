#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_rfid.h>

#include <lib/lfrfid/lfrfid_dict_file.h>
#include <lib/lfrfid/lfrfid_worker.h>

#include "lib/worker/helpers/hardware_worker.h"


// #include <lfrfid/protocols/lfrfid_protocols.h>  <- this works but not the one below
// #include <lfrfid/protocols/protocol_hid_generic.h> or #include <lib/lfrfid/protocols/protocol_hid_generic.h>

#include <gui/gui.h>
#include <input/input.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>

#include <notification/notification.h>
#include <notification/notification_messages.h>

#include "rfid_app_icons.h"

#include <gui/modules/byte_input.h>

#define TAG "RFID_APP"

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
    uint8_t current_offset;  
    uint8_t menu_selection;  
    uint8_t input_bytes[8];  
    ViewPort* byte_input_view_port; // ViewPort for data input -> TODO: Wanted ByteInput but not working
} RfidApp;

static void app_draw_callback(Canvas* canvas, void* ctx) {
    RfidApp* app = ctx;
    
    // Don't draw if we're showing the byte input view (crashes)
    if(app->state == RfidAppStateInputData) {
        return;
    }
    
    canvas_clear(canvas);
    
    // Don't show the title when in menu state (save vspace)
    if(app->state != RfidAppStateMenu) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 12, "RFID Tool");
    }
    
    canvas_set_font(canvas, FontSecondary);
    switch(app->state) {
        case RfidAppStateIdle:
            canvas_draw_str(canvas, 2, 24, "OK: Read, Up: Menu");
            canvas_draw_str(canvas, 2, 36, "Back: Exit");
            if(app->tag_found) {
                canvas_draw_str(canvas, 2, 48, "Tag data found!");
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
            canvas_draw_str(canvas, 2, 48, "Press Back to cancel");
            break;
        case RfidAppStateMenu:
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str(canvas, 2, 12, "Main Menu");
            canvas_set_font(canvas, FontSecondary);
            canvas_draw_str(canvas, 2, 24, app->menu_selection == 0 ? "> Set Offset" : "  Set Offset");
            canvas_draw_str(canvas, 2, 34, app->menu_selection == 1 ? "> Input Data" : "  Input Data");
            canvas_draw_str(canvas, 2, 44, app->menu_selection == 2 ? "> Write Tag" : "  Write Tag");
            canvas_draw_str(canvas, 2, 54, app->menu_selection == 3 ? "> Emulate Tag" : "  Emulate Tag");
            break;
        case RfidAppStateInputOffset:
            canvas_draw_str(canvas, 2, 24, "Enter Offset (0-255):");
            char offset_str[8];
            snprintf(offset_str, sizeof(offset_str), "%d", app->current_offset);
            canvas_draw_str(canvas, 2, 36, offset_str);
            break;
        case RfidAppStateInputData:
            // This case should not be reached now since we return early
            break;
    }
}

static void app_input_callback(InputEvent* input_event, void* ctx) {
    RfidApp* app = ctx;
    furi_message_queue_put(app->event_queue, input_event, FuriWaitForever);
}
// green fladh
static void beep() {
    NotificationApp* notification = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notification, &sequence_success);
    furi_record_close(RECORD_NOTIFICATION);
}
// red flash
static void error_beep() {
    NotificationApp* notification = furi_record_open(RECORD_NOTIFICATION);
    notification_message(notification, &sequence_error);
    furi_record_close(RECORD_NOTIFICATION);
}
// format: update state before method call but clean up by changing state back at end of method
static void rfid_read_callback(LFRFIDWorkerReadResult result, ProtocolId protocol, void* context) {
    RfidApp* app = context;
    if(result == LFRFIDWorkerReadDone) {
        // Get the protocol data
        size_t data_size = protocol_dict_get_data_size(app->protocols, protocol);
        protocol_dict_get_data(app->protocols, protocol, app->tag_data, data_size);
        
        app->tag_found = true;
        // cleanup actions
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
        app->state = RfidAppStateIdle;
        beep();
    } else {
        app->state = RfidAppStateIdle;
        error_beep();
        // TODO: retry/error handling
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

static void rfid_emulate_tag(RfidApp* app) {
    if(!app->tag_found) {
        error_beep();
        return;
    }

    app->state = RfidAppStateEmulating;
    
    // Make sure the data is in the protocol dictionary
    protocol_dict_set_data(app->protocols, LFRFIDProtocolHidGeneric, app->tag_data, 8);
    
    // Start emulation - no callback needed
    lfrfid_worker_emulate_start(app->worker, LFRFIDProtocolHidGeneric);
}

static void handle_menu_input(RfidApp* app, InputEvent* event) {
    if(event->type == InputTypeShort) {
        switch(event->key) {
            case InputKeyUp:
                if(app->menu_selection > 0) app->menu_selection--;
                break;
            case InputKeyDown:
                if(app->menu_selection < 3) app->menu_selection++;
                break;
            case InputKeyOk:
                switch(app->menu_selection) {
                    case 0:
                        app->state = RfidAppStateInputOffset;
                        break;
                    case 1:
                        app->state = RfidAppStateInputData;
                        
                        // Initialize input data
                        if(app->tag_found) {
                            memcpy(app->input_bytes, app->tag_data, 8);
                        } else {
                            memset(app->input_bytes, 0, 8);
                        }
                        
                        break;
                    case 2:
                        app->state = RfidAppStateWriting;
                        rfid_write_tag(app);
                        break;
                    case 3:
                        if(app->tag_found) {
                            app->state = RfidAppStateEmulating;
                            rfid_emulate_tag(app);
                        } else {
                            error_beep(); // Notify user no tag data available
                        }
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

static void byte_input_view_port_draw_callback(Canvas* canvas, void* context) {
    RfidApp* app = context;
    
    canvas_clear(canvas);
    
    // Header
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "RFID Tag Data Input");
    
    // Draw the byte input UI
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 24, "Enter hexadecimal bytes");
    
    char hex_str[50] = {0}; // Larger buffer for safety
    char* pos = hex_str;
    int remaining = sizeof(hex_str);
    int chars_written;
    
    for(int i = 0; i < 8; i++) {
        if(i == app->current_offset % 8) {
            chars_written = snprintf(pos, remaining, ">%02X< ", app->input_bytes[i]);
        } else {
            chars_written = snprintf(pos, remaining, "%02X ", app->input_bytes[i]);
        }
        
        if(chars_written > 0 && chars_written < remaining) {
            pos += chars_written;
            remaining -= chars_written;
        } else {
            // TODO: Buffer is full or error occurred -> handle?
            break;
        }
    }
    
    canvas_draw_str(canvas, 2, 36, hex_str);
    
    canvas_draw_str(canvas, 2, 48, "Up/Down: Change value");
    canvas_draw_str(canvas, 2, 60, "Left/Right: Move | OK: Done");
}

static void byte_input_view_port_input_callback(InputEvent* event, void* context) {
    RfidApp* app = context;
    
    // Handle input for our custom byte input UI
    bool consumed = false;
    uint8_t current_byte = 0;
    uint8_t modified_byte = 0;
    
    if(event->type == InputTypeShort || event->type == InputTypeRepeat) {
        switch(event->key) {
            case InputKeyOk:
                // Complete input, return to menu
                memcpy(app->tag_data, app->input_bytes, 8);
                app->tag_found = true;  // We now have valid data
                app->state = RfidAppStateMenu;
                consumed = true;
                break;
                
            case InputKeyUp:
                // Increment current byte
                current_byte = app->input_bytes[app->current_offset % 8];
                modified_byte = current_byte + 1;
                app->input_bytes[app->current_offset % 8] = modified_byte;
                consumed = true;
                break;
                
            case InputKeyDown:
                // Decrement current byte
                current_byte = app->input_bytes[app->current_offset % 8];
                modified_byte = current_byte - 1;
                app->input_bytes[app->current_offset % 8] = modified_byte;
                consumed = true;
                break;
                
            case InputKeyLeft:
                // Move to previous byte
                if(app->current_offset > 0) {
                    app->current_offset--;
                } else {
                    app->current_offset = 7;  // Wrap around to last byte
                }
                consumed = true;
                break;
                
            case InputKeyRight:
                // Move to next byte
                if(app->current_offset < 7) {
                    app->current_offset++;
                } else {
                    app->current_offset = 0;  // Wrap around to first byte
                }
                consumed = true;
                break;
                
            default:
                break;
        }
        
        if(consumed) {
            view_port_update(app->byte_input_view_port);
        }
    } else if(event->type == InputTypeLong && event->key == InputKeyBack) {
        // Long press back to exit without saving
        app->state = RfidAppStateMenu;
    }
}

int32_t rfid_app_main(void* p) {
    UNUSED(p);
    
    RfidApp* app = malloc(sizeof(RfidApp));
    app->state = RfidAppStateIdle;
    app->tag_found = false;
    app->status_text = furi_string_alloc();
    app->byte_input_view_port = NULL;
    
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
            if(event.type == InputTypeShort || event.type == InputTypeLong) {
                switch(app->state) {
                    case RfidAppStateIdle:
                        switch(event.key) {
                            case InputKeyOk:
                                rfid_read_tag(app);
                                break;
                            case InputKeyUp:
                                app->state = RfidAppStateMenu;
                                break;
                            case InputKeyDown:
                                if(app->tag_found) {
                                    app->state = RfidAppStateEmulating;
                                    rfid_emulate_tag(app);
                                }
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
                        if(event.key == InputKeyBack && event.type == InputTypeLong) {
                            // Long press back to exit without saving
                            app->state = RfidAppStateMenu;
                            
                            // Remove byte input view port and show main view port
                            if(app->byte_input_view_port) {
                                gui_remove_view_port(app->gui, app->byte_input_view_port);
                                gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
                            }
                        }
                        break;
                    case RfidAppStateEmulating:
                        if(event.key == InputKeyBack) {
                            lfrfid_worker_stop(app->worker);
                            app->state = RfidAppStateIdle;
                            beep(); // Notify user emulation has ended
                        }
                        break;
                    case RfidAppStateWriting:
                        if(event.key == InputKeyBack) {
                            lfrfid_worker_stop(app->worker);
                            app->state = RfidAppStateIdle;
                            furi_string_set(app->status_text, "Writing cancelled");
                            error_beep();
                        }
                        break;
                    default:
                        break;
                }
            }
        }
        
        // Handle view switching
        if(app->state == RfidAppStateInputData && app->byte_input_view_port == NULL) {
            // Switch to byte input view
            gui_remove_view_port(app->gui, app->view_port);
            
            // Create byte input ViewPort
            app->byte_input_view_port = view_port_alloc();
            view_port_draw_callback_set(app->byte_input_view_port, byte_input_view_port_draw_callback, app);
            view_port_input_callback_set(app->byte_input_view_port, byte_input_view_port_input_callback, app);
            gui_add_view_port(app->gui, app->byte_input_view_port, GuiLayerFullscreen);
            
        } else if(app->state != RfidAppStateInputData && app->byte_input_view_port != NULL) {
            // Switch back to main view
            gui_remove_view_port(app->gui, app->byte_input_view_port);
            view_port_free(app->byte_input_view_port);
            app->byte_input_view_port = NULL;
            gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
        }
        
        view_port_update(app->view_port);
        if(app->byte_input_view_port) {
            view_port_update(app->byte_input_view_port);
        }
    }
    
    // Cleanup
    lfrfid_worker_stop(app->worker);
    lfrfid_worker_stop_thread(app->worker);
    lfrfid_worker_free(app->worker);
    protocol_dict_free(app->protocols);
    view_port_enabled_set(app->view_port, false);
    gui_remove_view_port(app->gui, app->view_port);
    if(app->byte_input_view_port) {
        gui_remove_view_port(app->gui, app->byte_input_view_port);
        view_port_free(app->byte_input_view_port);
    }
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->event_queue);
    furi_string_free(app->status_text);
    free(app);
    
    return 0;
} 