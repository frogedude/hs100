#define _CRT_SECURE_NO_WARNINGS
#include <stddef.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>      /* for POSIX signals */
#include "comms.h"
#include "version.h"
#include "cJSON.h"

// Forward declaration for interactive mode
void interactive_mode(char* ip);

/* Windows compatibility */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>     /* for SetConsoleCtrlHandler */
#pragma comment(lib, "ws2_32.lib")
#define SOCKET_TYPE SOCKET
#define SOCKET_INVALID INVALID_SOCKET
#define SOCKET_ERR(x) ((x) == INVALID_SOCKET)
#define CLOSE_SOCKET(s) closesocket(s)
#define GET_LAST_ERROR WSAGetLastError()
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#define SOCKET_TYPE int
#define SOCKET_INVALID -1
#define SOCKET_ERR(x) ((x) < 0)
#define CLOSE_SOCKET(s) close(s)
#define GET_LAST_ERROR errno
#endif

#define RECV_BUF_SIZE 4096

/* Global flag for graceful shutdown */
volatile sig_atomic_t keep_running = 1;

/* Handlers declared elsewhere */
extern char* handler_associate(int argc, char* argv[]);
extern char* handler_set_server(int argc, char* argv[]);
extern char* handler_set_relay_state(int argc, char* argv[]);
extern char* handler_get_realtime(int argc, char* argv[]);
extern char* handler_set_alias(int argc, char* argv[]);
extern char* handler_outlet(int argc, char* argv[]);
extern char* handler_interactive(int argc, char* argv[]);

/* Console handler prototypes */
#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD dwCtrlType);
#else
static void signal_handler(int sig);
#endif

struct cmd_s {
    char* command;
    char* help;
    char* json;                       /* fallback JSON if no handler */
    char* (*handler)(int argc, char* argv[]);
    int no_response;                  /* if 1, NULL response is not an error */
};

struct cmd_s cmds[] = {
    {
        .command = "associate",
        .help = "associate <ssid> <key> <key_type>\n"
                "\t\t\tset wifi AP to connect to",
        .handler = handler_associate,
    },
    {
        .command = "emeter",
        .help = "emeter\t\tget realtime power consumption (only works with HS110)",
        .handler = handler_get_realtime,
        .json = "{\"emeter\":{\"get_realtime\":{}}}",
    },
    {
        .command = "factory-reset",
        .help = "factory-reset\treset the plug to factory settings",
        .json = "{\"system\":{\"reset\":{\"delay\":0}}}",
    },
    {
        .command = "info",
        .help = "info\t\tget device info",
        .json = "{\"system\":{\"get_sysinfo\":{}}}",
    },
    {
        .command = "off",
        .help = "off\t\tturn the plug off",
        .handler = handler_set_relay_state,
        .json = "{\"system\":{\"set_relay_state\":{\"state\":0}}}",
    },
    {
        .command = "on",
        .help = "on\t\tturn the plug on",
        .handler = handler_set_relay_state,
        .json = "{\"system\":{\"set_relay_state\":{\"state\":1}}}",
    },
    {
        .command = "reboot",
        .help = "reboot\t\treboot the plug",
        .json = "{\"system\":{\"reboot\":{\"delay\":0}}}",
    },
    {
        .command = "scan",
        .help = "scan\t\tscan for nearby wifi APs (probably only 2.4 GHz ones)",
        .json = "{\"netif\":{\"get_scaninfo\":{\"refresh\":1}}}",
    },
    {
        .command = "set_server",
        .help = "set_server <url>\n"
                "\t\t\tset cloud server to <url> instead of tplink's",
        .handler = handler_set_server,
    },
    {
        .command = "alias",
        .help = "alias <name>\n"
                "\t\t\tset device alias to <name>",
        .handler = handler_set_alias,
    },
    {
        .command = "outlet",
        .help = "outlet <num> <on|off>\n"
                "\t\t\tcontrol a specific outlet (1-6)",
        .handler = handler_outlet,
    },
    {
        .command = "interactive",
        .help = "interactive\tenter interactive mode to toggle outlets",
        .handler = handler_interactive,
        .no_response = 1,
    },
    {.command = NULL },
};

struct cmd_s* get_cmd_from_name(char* needle) {
    int i = 0;
    while (cmds[i].command) {
        if (!strcmp(cmds[i].command, needle))
            return &cmds[i];
        i++;
    }
    return NULL;
}

void print_usage() {
    fprintf(stderr,
        "hs100 version " VERSION_STRING
        ", Copyright (C) 2018-2019 Jason Benaim.\n"
        "A tool for using certain wifi smart plugs.\n\n"
        "usage: hs100 <ip> <command>\n\n"
        "Commands:\n"
    );
    int i = 0;
    while (cmds[i].command) {
        fprintf(stderr, "\t%s\n\n", cmds[i].help);
        i++;
    }
    fprintf(stderr, "Report bugs to https://github.com/jkbenaim/hs100\n");
}

/* ------------------------------------------------------------------------- */
/* Console handler implementations                                           */
/* ------------------------------------------------------------------------- */
#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD dwCtrlType) {
    (void)dwCtrlType;   /* unused parameter */
    keep_running = 0;
    /* Return TRUE to indicate we handled the event.
       The system may still terminate the process, but we set the flag
       so that interactive_mode can exit cleanly. */
    return TRUE;
}
#else
static void signal_handler(int sig) {
    (void)sig;   /* unused */
    keep_running = 0;
}
#endif

/* ------------------------------------------------------------------------- */
/* Interactive mode: periodically display outlets and allow relay toggling   */
/* ------------------------------------------------------------------------- */
void interactive_mode(char* ip) {
    int keep_going = 1;
    char input[10];
    int choice;

    while (keep_running && keep_going) {
        /* Fetch device info */
        char* info_response = hs100_send(ip, "{\"system\":{\"get_sysinfo\":{}}}");
        if (!info_response) {
            fprintf(stderr, "Failed to get device info\n");
            return;
        }

        cJSON* json = cJSON_Parse(info_response);
        free(info_response);
        if (!json) {
            fprintf(stderr, "Invalid JSON from device\n");
            return;
        }

        cJSON* sysinfo = cJSON_GetObjectItem(json, "system");
        if (sysinfo) sysinfo = cJSON_GetObjectItem(sysinfo, "get_sysinfo");
        if (!sysinfo) {
            fprintf(stderr, "No sysinfo in response\n");
            cJSON_Delete(json);
            return;
        }

        /* Safely read device info */
        cJSON* alias_item = cJSON_GetObjectItem(sysinfo, "alias");
        cJSON* model_item = cJSON_GetObjectItem(sysinfo, "model");
        cJSON* mac_item = cJSON_GetObjectItem(sysinfo, "mac");
        printf("\nDevice: %s\n", alias_item && alias_item->valuestring ? alias_item->valuestring : "?");
        printf("Model:  %s\n", model_item && model_item->valuestring ? model_item->valuestring : "?");
        printf("MAC:    %s\n\n", mac_item && mac_item->valuestring ? mac_item->valuestring : "?");

        cJSON* children = cJSON_GetObjectItem(sysinfo, "children");
        int child_count = 0;
        cJSON** child_list = NULL;
        int* states = NULL;
        char** child_ids = NULL;
        char** child_aliases = NULL;

        if (children && cJSON_IsArray(children)) {
            child_count = cJSON_GetArraySize(children);
            if (child_count > 0) {
                /* Allocate arrays */
                child_list = malloc(child_count * sizeof(cJSON*));
                states = malloc(child_count * sizeof(int));
                child_ids = malloc(child_count * sizeof(char*));
                child_aliases = malloc(child_count * sizeof(char*));
                if (!child_list || !states || !child_ids || !child_aliases) {
                    fprintf(stderr, "Memory allocation failed\n");
                    free(child_list); free(states); free(child_ids); free(child_aliases);
                    cJSON_Delete(json);
                    return;
                }
                for (int i = 0; i < child_count; i++) {
                    child_list[i] = cJSON_GetArrayItem(children, i);
                    cJSON* state_item = cJSON_GetObjectItem(child_list[i], "state");
                    states[i] = state_item ? state_item->valueint : 0;
                    cJSON* id_item = cJSON_GetObjectItem(child_list[i], "id");
                    child_ids[i] = id_item && id_item->valuestring ? id_item->valuestring : "";
                    cJSON* alias_item_child = cJSON_GetObjectItem(child_list[i], "alias");
                    child_aliases[i] = alias_item_child && alias_item_child->valuestring ? alias_item_child->valuestring : "";
                }
            }
        }

        if (child_count > 0) {
            /* Multi-outlet device */
            printf("Outlets:\n");
            printf(" # | ID                                         |State| Alias\n");
            printf("---+--------------------------------------------+-----+------------------\n");
            for (int i = 0; i < child_count; i++) {
                printf("%2d | %-42s | %-3s | %s\n",
                    i + 1,
                    child_ids[i],
                    states[i] ? "ON" : "OFF",
                    child_aliases[i]);
            }

            printf("\nEnter outlet number to toggle the relay (1-%d) or just Enter to exit: ", child_count);
            if (fgets(input, sizeof(input), stdin) == NULL) {
                /* Input error – possibly due to signal interruption */
                if (!keep_running) break;
                clearerr(stdin);
                continue;
            }
            if (input[0] == '\n') {
                keep_going = 0;
            }
            else {
                choice = atoi(input);
                if (choice >= 1 && choice <= child_count) {
                    #ifdef _WIN32
                    system("cls");
                    #else
                    printf("\033[2J\033[H");
                    #endif
                    int idx = choice - 1;
                    int new_state = (states[idx] == 0) ? 1 : 0;

                    /* Build command using cJSON (safe) */
                    cJSON* root = cJSON_CreateObject();
                    cJSON* context = cJSON_CreateObject();
                    cJSON* child_ids_arr = cJSON_CreateArray();
                    cJSON_AddItemToArray(child_ids_arr, cJSON_CreateString(child_ids[idx]));
                    cJSON_AddItemToObject(context, "child_ids", child_ids_arr);
                    cJSON_AddItemToObject(root, "context", context);
                    cJSON* system = cJSON_CreateObject();
                    cJSON* set_relay_state = cJSON_CreateObject();
                    cJSON_AddNumberToObject(set_relay_state, "state", new_state);
                    cJSON_AddItemToObject(system, "set_relay_state", set_relay_state);
                    cJSON_AddItemToObject(root, "system", system);

                    char* msg = cJSON_PrintUnformatted(root);
                    cJSON_Delete(root);
                    if (msg) {
                        char* response = hs100_send(ip, msg);
                        free(msg);
                        if (response) free(response);
                        else fprintf(stderr, "Failed to toggle relay on outlet %d\n", choice);
                    }
                    else {
                        fprintf(stderr, "Failed to build JSON command\n");
                    }
                }
                else {
                    fprintf(stderr, "Invalid choice.\n");
                }
            }
        }
        else {
            /* Single-outlet device */
            cJSON* relay_state_item = cJSON_GetObjectItem(sysinfo, "relay_state");
            int relay_state = relay_state_item ? relay_state_item->valueint : 0;
            printf("Main relay state: %s\n", relay_state ? "ON" : "OFF");
            printf("\nEnter 1 to toggle the relay, or just Enter to exit: ");
            if (fgets(input, sizeof(input), stdin) == NULL) {
                if (!keep_running) break;
                clearerr(stdin);
                continue;
            }
            if (input[0] == '\n') {
                keep_going = 0;
            }
            else {
                choice = atoi(input);
                if (choice == 1) {
                    int new_state = (relay_state == 0) ? 1 : 0;

                    /* Build command with cJSON */
                    cJSON* root = cJSON_CreateObject();
                    cJSON* system = cJSON_CreateObject();
                    cJSON* set_relay_state = cJSON_CreateObject();
                    cJSON_AddNumberToObject(set_relay_state, "state", new_state);
                    cJSON_AddItemToObject(system, "set_relay_state", set_relay_state);
                    cJSON_AddItemToObject(root, "system", system);

                    char* msg = cJSON_PrintUnformatted(root);
                    cJSON_Delete(root);
                    if (msg) {
                        char* response = hs100_send(ip, msg);
                        free(msg);
                        if (response) free(response);
                        else fprintf(stderr, "Failed to toggle relay\n");
                    }
                    else {
                        fprintf(stderr, "Failed to build JSON command\n");
                    }
                }
                else {
                    fprintf(stderr, "Invalid choice.\n");
                }
            }
        }

        /* Cleanup */
        free(child_list);
        free(states);
        free(child_ids);
        free(child_aliases);
        cJSON_Delete(json);
    }
}

/* ------------------------------------------------------------------------- */
/* Helper: get child ID for a numeric outlet index (1‑based)                 */
/* Returns a newly allocated string; caller must free().                     */
/* ------------------------------------------------------------------------- */
static char* get_child_id_from_index(char* ip, int idx) {
    char* sysinfo_json = hs100_send(ip, "{\"system\":{\"get_sysinfo\":{}}}");
    if (!sysinfo_json) return NULL;

    cJSON* root = cJSON_Parse(sysinfo_json);
    free(sysinfo_json);
    if (!root) return NULL;

    cJSON* sysinfo = cJSON_GetObjectItem(root, "system");
    if (sysinfo) sysinfo = cJSON_GetObjectItem(sysinfo, "get_sysinfo");
    if (!sysinfo) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON* children = cJSON_GetObjectItem(sysinfo, "children");
    if (!children || !cJSON_IsArray(children)) {
        cJSON_Delete(root);
        return NULL;
    }

    int child_count = cJSON_GetArraySize(children);
    if (idx < 1 || idx > child_count) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON* child = cJSON_GetArrayItem(children, idx - 1);
    cJSON* id_obj = cJSON_GetObjectItem(child, "id");
    if (!id_obj || !id_obj->valuestring) {
        cJSON_Delete(root);
        return NULL;
    }

    char* child_id = strdup(id_obj->valuestring);
    cJSON_Delete(root);
    return child_id;
}

/* ------------------------------------------------------------------------- */
/* Helper: get numeric index (1‑based) for a given child ID                  */
/* Returns 0 if not found.                                                   */
/* ------------------------------------------------------------------------- */
static int get_index_from_child_id(char* ip, const char* child_id) {
    char* sysinfo_json = hs100_send(ip, "{\"system\":{\"get_sysinfo\":{}}}");
    if (!sysinfo_json) return 0;

    cJSON* root = cJSON_Parse(sysinfo_json);
    free(sysinfo_json);
    if (!root) return 0;

    cJSON* sysinfo = cJSON_GetObjectItem(root, "system");
    if (sysinfo) sysinfo = cJSON_GetObjectItem(sysinfo, "get_sysinfo");
    if (!sysinfo) {
        cJSON_Delete(root);
        return 0;
    }

    cJSON* children = cJSON_GetObjectItem(sysinfo, "children");
    if (!children || !cJSON_IsArray(children)) {
        cJSON_Delete(root);
        return 0;
    }

    int child_count = cJSON_GetArraySize(children);
    int index = 0;
    for (int i = 0; i < child_count; i++) {
        cJSON* child = cJSON_GetArrayItem(children, i);
        cJSON* id_obj = cJSON_GetObjectItem(child, "id");
        if (id_obj && id_obj->valuestring && strcmp(id_obj->valuestring, child_id) == 0) {
            index = i + 1;
            break;
        }
    }

    cJSON_Delete(root);
    return index;
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */
int main(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage();
        return 1;
    }

    /* Install console signal handlers */
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* do not restart interrupted system calls */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#endif

    char* plug_addr = argv[1];
    char* cmd_string = argv[2];
    char* response = NULL;

    /* --- Variables to support transformed "on <num>" / "off <num>" --- */
    char* original_cmd = NULL;      /* original command ("on" or "off") if transformed */
    int original_outlet_num = 0;    /* numeric outlet index (1..6) if transformed */
    char* allocated_child_id = NULL;/* child ID allocated by get_child_id_from_index, to be freed */

    /* --- Transform "on <num>" / "off <num>" into "on" with a child ID --- */
    if ((strcmp(cmd_string, "on") == 0 || strcmp(cmd_string, "off") == 0) && argc >= 4) {
        char* outlet_arg = argv[3];
        int outlet_num = atoi(outlet_arg);
        /* Assume max 6 outlets; adjust if needed */
        if (outlet_num >= 1 && outlet_num <= 6) {
            char* child_id = get_child_id_from_index(plug_addr, outlet_num);
            if (child_id) {
                allocated_child_id = child_id;   /* remember to free later */
                original_cmd = cmd_string;
                original_outlet_num = outlet_num;
                argv[3] = child_id;              /* replace argument with the hex ID */
            }
            else {
                fprintf(stderr, "Failed to resolve outlet number %d to child ID\n", outlet_num);
                return 1;
            }
        }
        else {
            /* The argument is already a hex ID – no transformation needed */
        }
    }

    /* --- Look up and execute the command --- */
    struct cmd_s* cmd = get_cmd_from_name(cmd_string);
    if (cmd != NULL) {
        if (cmd->handler != NULL) {
            response = cmd->handler(argc, argv);
        }
        /* Only use the static JSON if there is no handler at all */
        if (response == NULL && cmd->handler == NULL && cmd->json != NULL) {
            response = hs100_send(plug_addr, cmd->json);
        }
    }
    else {
        /* Unknown command – send raw string to the plug */
        response = hs100_send(plug_addr, cmd_string);
    }

    /* --- Handle errors / no response --- */
    if (response == NULL) {
        if (cmd == NULL || !cmd->no_response) {
            fprintf(stderr, "failed to send command\n");
            if (allocated_child_id) free(allocated_child_id);
            return 1;
        }
        if (allocated_child_id) free(allocated_child_id);
        return 0;   /* handled successfully (e.g., interactive) */
    }

    /* --- Format output --- */
    if (strcmp(cmd_string, "info") == 0) {
        /* info command: pretty‑print the device and outlet table (existing code) */
        cJSON* json = cJSON_Parse(response);
        if (json) {
            cJSON* sysinfo = cJSON_GetObjectItem(json, "system");
            if (sysinfo) sysinfo = cJSON_GetObjectItem(sysinfo, "get_sysinfo");
            if (sysinfo) {
                cJSON* alias = cJSON_GetObjectItem(sysinfo, "alias");
                cJSON* model = cJSON_GetObjectItem(sysinfo, "model");
                cJSON* mac = cJSON_GetObjectItem(sysinfo, "mac");
                printf("Device: %s\n", alias && alias->valuestring ? alias->valuestring : "?");
                printf("Model:  %s\n", model && model->valuestring ? model->valuestring : "?");
                printf("MAC:    %s\n\n", mac && mac->valuestring ? mac->valuestring : "?");

                cJSON* children = cJSON_GetObjectItem(sysinfo, "children");
                if (children && cJSON_IsArray(children)) {
                    int child_count = cJSON_GetArraySize(children);
                    printf("Outlets:\n");
                    printf(" # | ID                                         |State| Alias\n");
                    printf("---+--------------------------------------------+-----+------------------\n");
                    for (int i = 0; i < child_count; i++) {
                        cJSON* child = cJSON_GetArrayItem(children, i);
                        cJSON* id = cJSON_GetObjectItem(child, "id");
                        cJSON* alias_child = cJSON_GetObjectItem(child, "alias");
                        cJSON* state = cJSON_GetObjectItem(child, "state");
                        printf("%2d | %-42s | %-3s | %s\n",
                            i + 1,
                            id && id->valuestring ? id->valuestring : "?",
                            state && state->valueint ? "ON" : "OFF",
                            alias_child && alias_child->valuestring ? alias_child->valuestring : "");
                    }
                }
                else {
                    cJSON* relay_state = cJSON_GetObjectItem(sysinfo, "relay_state");
                    printf("Relay state: %s\n", relay_state && relay_state->valueint ? "ON" : "OFF");
                }
            }
            cJSON_Delete(json);
        }
        else {
            printf("%s\n", response);
        }
    }
    else {
        /* For all other commands, parse JSON and print user‑friendly output */
        cJSON* root = cJSON_Parse(response);
        if (root) {
            int printed = 0;

            /* --- Special case: we transformed an "on <num>" / "off <num>" command --- */
            if (original_cmd && original_outlet_num > 0) {
                cJSON* sys = cJSON_GetObjectItem(root, "system");
                if (sys) {
                    cJSON* relay = cJSON_GetObjectItem(sys, "set_relay_state");
                    if (relay) {
                        cJSON* err = cJSON_GetObjectItem(relay, "err_code");
                        if (err && err->valueint == 0) {
                            printf("Outlet %d turned %s.\n", original_outlet_num,
                                strcmp(original_cmd, "on") == 0 ? "on" : "off");
                        }
                        else {
                            printf("Outlet %d command failed with error code: %d\n",
                                original_outlet_num, err ? err->valueint : -1);
                        }
                        printed = 1;
                    }
                }
            }

            /* --- Special case for 'outlet' command --- */
            if (!printed && strcmp(cmd_string, "outlet") == 0) {
                cJSON* sys = cJSON_GetObjectItem(root, "system");
                if (sys) {
                    cJSON* relay = cJSON_GetObjectItem(sys, "set_relay_state");
                    if (relay) {
                        cJSON* err = cJSON_GetObjectItem(relay, "err_code");
                        if (err && err->valueint == 0) {
                            int outlet_num = 0;
                            char* outlet_arg = argv[3];
                            int num = atoi(outlet_arg);
                            if (num >= 1 && num <= 6) {
                                outlet_num = num;
                            }
                            else {
                                /* It's a hex ID – try to find its index */
                                outlet_num = get_index_from_child_id(plug_addr, outlet_arg);
                            }
                            if (outlet_num > 0) {
                                printf("Outlet %d turned %s.\n", outlet_num,
                                    (strcmp(argv[4], "on") == 0) ? "on" : "off");
                            }
                            else {
                                printf("Outlet command executed successfully.\n");
                            }
                        }
                        else {
                            printf("Outlet command failed with error code: %d\n",
                                err ? err->valueint : -1);
                        }
                        printed = 1;
                    }
                }
            }

            /* --- 1) Generic commands that return system.*.err_code (on, off, reboot, etc.) --- */
            if (!printed) {
                cJSON* sys = cJSON_GetObjectItem(root, "system");
                if (sys) {
                    cJSON* relay = cJSON_GetObjectItem(sys, "set_relay_state");
                    cJSON* reboot = cJSON_GetObjectItem(sys, "reboot");
                    cJSON* reset = cJSON_GetObjectItem(sys, "reset");
                    cJSON* cmd_obj = relay ? relay : (reboot ? reboot : reset);
                    if (cmd_obj) {
                        cJSON* err = cJSON_GetObjectItem(cmd_obj, "err_code");
                        if (err && err->valueint == 0) {
                            printf("Command '%s' executed successfully.\n", cmd_string);
                        }
                        else {
                            printf("Command '%s' failed with error code: %d\n",
                                cmd_string, err ? err->valueint : -1);
                        }
                        printed = 1;
                    }
                }
            }

            /* --- 2) scan command: format access point list --- */
            if (!printed) {
                cJSON* netif = cJSON_GetObjectItem(root, "netif");
                if (netif) {
                    cJSON* scaninfo = cJSON_GetObjectItem(netif, "get_scaninfo");
                    if (scaninfo) {
                        cJSON* ap_list = cJSON_GetObjectItem(scaninfo, "ap_list");
                        if (ap_list && cJSON_IsArray(ap_list)) {
                            int count = cJSON_GetArraySize(ap_list);
                            printf("Found %d Wi-Fi networks:\n", count);
                            printf(" %-32s %-10s\n", "SSID", "Security");
                            printf(" %-32s %-10s\n", "--------------------------------", "----------");
                            for (int i = 0; i < count; i++) {
                                cJSON* ap = cJSON_GetArrayItem(ap_list, i);
                                cJSON* ssid = cJSON_GetObjectItem(ap, "ssid");
                                cJSON* key_type = cJSON_GetObjectItem(ap, "key_type");

                                const char* security = "?";
                                if (key_type) {
                                    int kt = key_type->valueint;
                                    switch (kt) {
                                    case 0: security = "Open"; break;
                                    case 1: security = "WEP"; break;
                                    case 2: security = "WPA"; break;
                                    case 3: security = "WPA2"; break;
                                    case 4: security = "WPA3"; break;
                                    default: security = "Other"; break;
                                    }
                                }

                                printf(" %-32s %-10s\n",
                                    ssid && ssid->valuestring ? ssid->valuestring : "(hidden)",
                                    security);
                            }
                            printed = 1;
                        }
                    }
                }
            }

            /* --- 3) emeter command: show power consumption --- UNTESTED */
            if (!printed) {
                cJSON* emeter = cJSON_GetObjectItem(root, "emeter");
                if (emeter) {
                    cJSON* realtime = cJSON_GetObjectItem(emeter, "get_realtime");
                    if (realtime) {
                        cJSON* voltage = cJSON_GetObjectItem(realtime, "voltage_mv");
                        cJSON* current = cJSON_GetObjectItem(realtime, "current_ma");
                        cJSON* power = cJSON_GetObjectItem(realtime, "power_mw");
                        cJSON* total = cJSON_GetObjectItem(realtime, "total_wh");
                        printf("Power consumption:\n");
                        if (voltage) printf("  Voltage: %.2f V\n", voltage->valuedouble / 1000.0);
                        if (current) printf("  Current: %.2f A\n", current->valuedouble / 1000.0);
                        if (power)   printf("  Power:   %.2f W\n", power->valuedouble / 1000.0);
                        if (total)   printf("  Total:   %.2f kWh\n", total->valuedouble / 1000.0);
                        printed = 1;
                    }
                }
            }

            /* --- If none of the above matched, print the raw JSON --- */
            if (!printed) {
                printf("%s\n", response);
            }

            cJSON_Delete(root);
        }
        else {
            /* Not valid JSON – print raw */
            printf("%s\n", response);
        }
    }

    /* --- Clean up --- */
    free(response);
    if (allocated_child_id) {
        free(allocated_child_id);
    }
    return 0;
}