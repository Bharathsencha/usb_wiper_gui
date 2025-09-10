#define RAYGUI_IMPLEMENTATION
#include "raylib.h"
#include "raygui.h"

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/mount.h>

// ----------------- Backend Functions ------------------

static int parse_kv(const char *line, const char *key, char *out, size_t outsize) {
    const char *p = strstr(line, key);
    if (!p) { 
        out[0] = '\0'; 
        return 0; 
    }
    
    p = strchr(p, '=');
    if (!p) { 
        out[0] = '\0'; 
        return 0; 
    }
    p++;
    
    // Handle quoted values
    if (*p == '"') {
        p++;
        const char *q = strchr(p, '"');
        if (!q) { 
            out[0] = '\0'; 
            return 0; 
        }
        size_t len = (size_t)(q - p);
        if (len >= outsize) len = outsize - 1;
        memcpy(out, p, len);
        out[len] = '\0';
        return 1;
    }
    
    // Handle unquoted values
    const char *end = p;
    while (*end && *end != ' ' && *end != '\n' && *end != '\r') {
        end++;
    }
    size_t len = (size_t)(end - p);
    if (len >= outsize) len = outsize - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

static int rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) 
                continue;
            
            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            rm_rf(child);
        }
        closedir(d);
        return rmdir(path);
    } else {
        return unlink(path);
    }
}

static int remove_dir_contents(const char *mountdir) {
    DIR *d = opendir(mountdir);
    if (!d) return -1;
    
    struct dirent *ent;
    char child[PATH_MAX];
    int result = 0;
    
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) 
            continue;
        
        snprintf(child, sizeof(child), "%s/%s", mountdir, ent->d_name);
        if (rm_rf(child) != 0) {
            result = -1;
        }
    }
    closedir(d);
    return result;
}

// Global variables for GUI
char logBuffer[16384] = {0};
bool isScanning = false;
Vector2 scrollOffset = {0, 0};

int calculate_text_height(const char *text, int fontSize) {
    if (!text || strlen(text) == 0) return 0;
    
    int lines = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') lines++;
    }
    return lines * (fontSize + 2);
}

void append_log(const char *msg) {
    size_t currentLen = strlen(logBuffer);
    size_t msgLen = strlen(msg);
    
    // Check if we need to truncate old logs
    if (currentLen + msgLen + 2 > sizeof(logBuffer) - 1) {
        // Remove first half of the buffer
        size_t halfPoint = sizeof(logBuffer) / 2;
        char *newlinePos = strchr(logBuffer + halfPoint, '\n');
        if (newlinePos) {
            memmove(logBuffer, newlinePos + 1, strlen(newlinePos + 1) + 1);
        } else {
            logBuffer[0] = '\0';
        }
    }
    
    strncat(logBuffer, msg, sizeof(logBuffer) - strlen(logBuffer) - 1);
    strncat(logBuffer, "\n", sizeof(logBuffer) - strlen(logBuffer) - 1);
    
    // Auto-scroll to bottom
    scrollOffset.y = calculate_text_height(logBuffer, 14) - 300;
    if (scrollOffset.y < 0) scrollOffset.y = 0;
}

void clear_log() {
    logBuffer[0] = '\0';
    scrollOffset.y = 0;
}

// ----------------- USB Detection and Wiping ------------------

typedef struct {
    char name[64];
    char path[256];  // Increased size
    char size[64];   // Increased size
} USBDevice;

USBDevice usbDevices[16];
int usbDeviceCount = 0;

void scan_usb_devices() {
    usbDeviceCount = 0;
    append_log("[*] Scanning for USB devices...");
    
    FILE *fp = popen("lsblk -P -o NAME,TRAN,SIZE,TYPE -n", "r");
    if (!fp) {
        append_log("ERROR: Failed to execute lsblk command");
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp) && usbDeviceCount < 16) {
        char name[128] = {0}, tran[128] = {0}, size[128] = {0}, type[128] = {0};
        
        parse_kv(line, "NAME", name, sizeof(name));
        parse_kv(line, "TRAN", tran, sizeof(tran));
        parse_kv(line, "SIZE", size, sizeof(size));
        parse_kv(line, "TYPE", type, sizeof(type));
        
        // Only include USB disks (not partitions)
        if (strcmp(tran, "usb") == 0 && strcmp(type, "disk") == 0) {
            strncpy(usbDevices[usbDeviceCount].name, name, sizeof(usbDevices[usbDeviceCount].name) - 1);
            snprintf(usbDevices[usbDeviceCount].path, sizeof(usbDevices[usbDeviceCount].path), "/dev/%s", name);
            strncpy(usbDevices[usbDeviceCount].size, size, sizeof(usbDevices[usbDeviceCount].size) - 1);
            
            char msg[512];  // Increased buffer size
            snprintf(msg, sizeof(msg), "[*] Found USB device: %s (%s)", 
                    usbDevices[usbDeviceCount].path, size);
            append_log(msg);
            
            usbDeviceCount++;
        }
    }
    pclose(fp);

    if (usbDeviceCount == 0) {
        append_log("[!] No USB devices found");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "[*] Found %d USB device(s)", usbDeviceCount);
        append_log(msg);
    }
}

void wipe_usb_device(const char *devpath) {
    char msg[512];  // Increased buffer size
    snprintf(msg, sizeof(msg), "[*] Starting wipe of device: %s", devpath);
    append_log(msg);

    // Check all possible partitions (1-16)
    bool foundPartitions = false;
    for (int i = 1; i <= 16; i++) {
        char partpath[128];
        snprintf(partpath, sizeof(partpath), "%s%d", devpath, i);
        
        if (access(partpath, F_OK) != 0) continue;
        foundPartitions = true;

        snprintf(msg, sizeof(msg), "[*] Processing partition: %s", partpath);
        append_log(msg);

        const char *mountdir = "/tmp/usb_wipe_mount";
        
        // Create mount directory
        if (mkdir(mountdir, 0755) != 0 && errno != EEXIST) {
            append_log("[!] Failed to create mount directory");
            continue;
        }

        // Try to mount the partition
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "mount %s %s 2>/dev/null", partpath, mountdir);
        
        if (system(cmd) != 0) {
            snprintf(msg, sizeof(msg), "[!] Failed to mount %s (may be encrypted or corrupted)", partpath);
            append_log(msg);
            continue;
        }

        append_log("[*] Mounted successfully. Deleting contents...");

        // Remove all contents
        if (remove_dir_contents(mountdir) != 0) {
            append_log("[!] Some files could not be deleted (permissions/in-use)");
        } else {
            append_log("[✓] All accessible files deleted");
        }

        // Sync and unmount
        sync();
        snprintf(cmd, sizeof(cmd), "umount %s", mountdir);
        if (system(cmd) != 0) {
            append_log("[!] Warning: Failed to unmount cleanly");
        } else {
            append_log("[✓] Partition unmounted");
        }
    }

    if (!foundPartitions) {
        append_log("[!] No partitions found on this device");
    }

    // Remove mount directory
    rmdir("/tmp/usb_wipe_mount");
    
    snprintf(msg, sizeof(msg), "[✓] Completed processing device: %s", devpath);
    append_log(msg);
    append_log("=====================================");
}

void wipe_all_usb() {
    if (usbDeviceCount == 0) {
        append_log("[!] No USB devices to wipe. Please scan first.");
        return;
    }

    append_log("[*] Starting USB wipe operation...");
    append_log("=====================================");
    
    for (int i = 0; i < usbDeviceCount; i++) {
        wipe_usb_device(usbDevices[i].path);
    }
    
    append_log("[✓] USB wipe operation completed!");
}

// ----------------- GUI Frontend ------------------

int main(void) {
    InitWindow(1000, 700, "USB Wipe Tool v2.0");
    SetTargetFPS(60);

    bool showConfirmDialog = false;
    int selectedDevice = -1;

    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(RAYWHITE);

        // Title
        DrawText("USB Wipe Tool v2.0", 20, 20, 28, DARKBLUE);
        DrawText("WARNING: This tool will permanently delete all data on USB devices!", 
                 20, 55, 16, RED);

        // Control buttons
        if (GuiButton((Rectangle){20, 90, 120, 35}, "Scan USB")) {
            clear_log();
            scan_usb_devices();
        }
        
        if (GuiButton((Rectangle){150, 90, 120, 35}, "Clear Log")) {
            clear_log();
        }

        // USB device list
        DrawText("Detected USB Devices:", 20, 140, 18, BLACK);
        
        for (int i = 0; i < usbDeviceCount; i++) {
            Rectangle deviceRect = {20, 170 + i * 35, 600, 30};
            
            char deviceInfo[512];  // Increased buffer size
            snprintf(deviceInfo, sizeof(deviceInfo), "%s (%s)", 
                    usbDevices[i].path, usbDevices[i].size);
            
            if (GuiButton(deviceRect, deviceInfo)) {
                selectedDevice = i;
                showConfirmDialog = true;
            }
        }

        if (usbDeviceCount == 0) {
            DrawText("No USB devices found. Click 'Scan USB' to search.", 
                     20, 170, 16, GRAY);
        }

        // Wipe all button
        if (usbDeviceCount > 0) {
            if (GuiButton((Rectangle){20, 170 + usbDeviceCount * 35 + 10, 150, 35}, "Wipe All USB")) {
                selectedDevice = -1;
                showConfirmDialog = true;
            }
        }

        // Log display area
        int logY = 170 + (usbDeviceCount > 0 ? usbDeviceCount * 35 + 55 : 45);
        DrawText("Logs:", 20, logY, 18, BLACK);
        
        Rectangle logArea = {20, logY + 25, 950, GetScreenHeight() - logY - 45};
        DrawRectangleLines((int)logArea.x, (int)logArea.y, (int)logArea.width, (int)logArea.height, GRAY);
        
        // Scrollable log text
        BeginScissorMode((int)logArea.x + 5, (int)logArea.y + 5, 
                        (int)logArea.width - 10, (int)logArea.height - 10);
        
        Vector2 textPos = {logArea.x + 10, logArea.y + 10 - scrollOffset.y};
        DrawTextEx(GetFontDefault(), logBuffer, textPos, 14, 1, DARKGRAY);
        
        EndScissorMode();

        // Handle scrolling
        float wheel = GetMouseWheelMove();
        if (wheel != 0) {
            scrollOffset.y -= wheel * 30;
            float maxScroll = calculate_text_height(logBuffer, 14) - logArea.height + 20;
            if (maxScroll < 0) maxScroll = 0;
            if (scrollOffset.y < 0) scrollOffset.y = 0;
            if (scrollOffset.y > maxScroll) scrollOffset.y = maxScroll;
        }

        // Confirmation dialog
        if (showConfirmDialog) {
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.5f));
            
            Rectangle dialogRect = {GetScreenWidth()/2 - 200, GetScreenHeight()/2 - 100, 400, 200};
            DrawRectangleRec(dialogRect, WHITE);
            DrawRectangleLinesEx(dialogRect, 2, DARKGRAY);
            
            const char *confirmText = selectedDevice >= 0 ? 
                "Are you sure you want to wipe this USB device?" : 
                "Are you sure you want to wipe ALL USB devices?";
            
            DrawText("CONFIRMATION", (int)(dialogRect.x + 10), (int)(dialogRect.y + 10), 20, RED);
            DrawText(confirmText, (int)(dialogRect.x + 10), (int)(dialogRect.y + 40), 16, BLACK);
            DrawText("This action cannot be undone!", (int)(dialogRect.x + 10), (int)(dialogRect.y + 65), 16, RED);
            
            if (selectedDevice >= 0) {
                char deviceText[512];  // Increased buffer size
                snprintf(deviceText, sizeof(deviceText), "Device: %s", usbDevices[selectedDevice].path);
                DrawText(deviceText, (int)(dialogRect.x + 10), (int)(dialogRect.y + 90), 14, DARKGRAY);
            }
            
            if (GuiButton((Rectangle){dialogRect.x + 50, dialogRect.y + 130, 100, 35}, "Yes, Wipe")) {
                if (selectedDevice >= 0) {
                    wipe_usb_device(usbDevices[selectedDevice].path);
                } else {
                    wipe_all_usb();
                }
                showConfirmDialog = false;
            }
            
            if (GuiButton((Rectangle){dialogRect.x + 200, dialogRect.y + 130, 100, 35}, "Cancel")) {
                showConfirmDialog = false;
            }
        }

        EndDrawing();
    }

    CloseWindow();
    return 0;
}