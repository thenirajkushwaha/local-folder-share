#include "gui.hpp"
#include "network.hpp"
#include "file_util.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstring>
#include <algorithm>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

enum class AppMode {
    CLIENT, // Receiver (saves to ~/Downloads)
    SENDER  // Sender (select peer, drag-and-drop or select files)
};

struct UIState {
    AppMode mode = AppMode::CLIENT;
    std::vector<PeerInfo> peers;
    int selected_peer_idx = -1;
    std::string status_msg = "Ready. Listening for incoming transfers...";
    std::string current_transfer_item = "";
    uint64_t bytes_transferred = 0;
    uint64_t total_bytes = 0;
    bool is_transferring = false;
    std::mutex mtx;
};

static unsigned long get_hex_color(Display* dpy, int screen, unsigned char r, unsigned char g, unsigned char b) {
    Colormap cmap = DefaultColormap(dpy, screen);
    XColor col;
    col.red = (unsigned short)(r * 257);
    col.green = (unsigned short)(g * 257);
    col.blue = (unsigned short)(b * 257);
    col.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(dpy, cmap, &col)) {
        return col.pixel;
    }
    return BlackPixel(dpy, screen);
}

int gui_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::cerr << "Error: Could not open X11 Display. If running under Wayland, ensure XWayland is active or DISPLAY is set.\n";
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    int win_w = 680;
    int win_h = 520;

    // Create Main Window
    Window win = XCreateSimpleWindow(
        dpy, root,
        100, 100, win_w, win_h,
        1,
        get_hex_color(dpy, screen, 40, 44, 52),
        get_hex_color(dpy, screen, 26, 27, 38)
    );

    // Set Window Title & Properties
    XStoreName(dpy, win, "Arch Linux Peer Drop & Share");
    XSetIconName(dpy, win, "PeerShare");

    // Enable Events
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | StructureNotifyMask | PointerMotionMask);

    // Atoms for WM close and XDnD protocol
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    // XDnD Atoms
    Atom xdnd_aware = XInternAtom(dpy, "XdndAware", False);
    Atom xdnd_enter = XInternAtom(dpy, "XdndEnter", False);
    Atom xdnd_position = XInternAtom(dpy, "XdndPosition", False);
    Atom xdnd_status = XInternAtom(dpy, "XdndStatus", False);
    Atom xdnd_action_copy = XInternAtom(dpy, "XdndActionCopy", False);
    Atom xdnd_drop = XInternAtom(dpy, "XdndDrop", False);
    Atom xdnd_finished = XInternAtom(dpy, "XdndFinished", False);
    Atom xdnd_selection = XInternAtom(dpy, "XdndSelection", False);
    Atom text_uri_list = XInternAtom(dpy, "text/uri-list", False);
    Atom prop_xdnd_selection = XInternAtom(dpy, "PRIMARY", False);

    unsigned char xdnd_version = 5;
    XChangeProperty(dpy, win, xdnd_aware, XA_ATOM, 32, PropModeReplace, &xdnd_version, 1);

    // Create Graphic Context
    GC gc = XCreateGC(dpy, win, 0, nullptr);

    // Try loading font
    XFontStruct* font = XLoadQueryFont(dpy, "-*-inter-medium-r-*-*-14-*-*-*-*-*-*-*");
    if (!font) font = XLoadQueryFont(dpy, "-*-liberation sans-medium-r-*-*-14-*-*-*-*-*-*-*");
    if (!font) font = XLoadQueryFont(dpy, "-*-dejavu sans-medium-r-*-*-14-*-*-*-*-*-*-*");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (font) XSetFont(dpy, gc, font->fid);

    // Colors
    unsigned long col_bg = get_hex_color(dpy, screen, 26, 27, 38);
    unsigned long col_card = get_hex_color(dpy, screen, 36, 40, 59);
    unsigned long col_card_border = get_hex_color(dpy, screen, 65, 72, 104);
    unsigned long col_accent = get_hex_color(dpy, screen, 122, 162, 247);      // Blue
    unsigned long col_accent_green = get_hex_color(dpy, screen, 158, 206, 106); // Green
    unsigned long col_accent_purple = get_hex_color(dpy, screen, 187, 154, 247);
    unsigned long col_text = get_hex_color(dpy, screen, 192, 202, 245);
    unsigned long col_text_dim = get_hex_color(dpy, screen, 120, 130, 160);
    unsigned long col_btn_active = get_hex_color(dpy, screen, 61, 89, 161);
    unsigned long col_btn_inactive = get_hex_color(dpy, screen, 41, 46, 66);
    unsigned long col_drop_hover = get_hex_color(dpy, screen, 49, 56, 84);

    XMapWindow(dpy, win);

    // State & Network
    UIState ui_state;
    NetworkManager net;

    // Callback for peer list updates
    net.set_peer_callback([&ui_state, dpy, win](const std::vector<PeerInfo>& peers) {
        {
            std::lock_guard<std::mutex> lock(ui_state.mtx);
            ui_state.peers = peers;
            if (ui_state.selected_peer_idx >= static_cast<int>(peers.size())) {
                ui_state.selected_peer_idx = peers.empty() ? -1 : 0;
            }
            if (ui_state.selected_peer_idx == -1 && !peers.empty()) {
                ui_state.selected_peer_idx = 0;
            }
        }
        // Send dummy expose event to trigger redraw
        XEvent ev{};
        ev.type = Expose;
        ev.xexpose.window = win;
        XSendEvent(dpy, win, False, ExposureMask, &ev);
        XFlush(dpy);
    });

    // Start discovery and start receiver server by default
    net.start_discovery(true);

    auto progress_callback = [&ui_state, dpy, win](const std::string& item, uint64_t bytes, uint64_t total, bool done, const std::string& err) {
        {
            std::lock_guard<std::mutex> lock(ui_state.mtx);
            if (!err.empty()) {
                ui_state.status_msg = "Error: " + err;
                ui_state.is_transferring = false;
            } else if (done) {
                ui_state.status_msg = item + " (" + FileUtil::format_bytes(bytes) + " transferred)";
                ui_state.is_transferring = false;
                ui_state.current_transfer_item = "";
                ui_state.bytes_transferred = bytes;
                ui_state.total_bytes = total;
            } else {
                ui_state.is_transferring = true;
                ui_state.current_transfer_item = item;
                ui_state.bytes_transferred = bytes;
                ui_state.total_bytes = total;
            }
        }
        XEvent ev{};
        ev.type = Expose;
        ev.xexpose.window = win;
        XSendEvent(dpy, win, False, ExposureMask, &ev);
        XFlush(dpy);
    };

    net.start_server(FileUtil::get_downloads_dir(), progress_callback);

    Window drag_source_win = None;
    bool is_drag_over = false;

    // Helper redraw function
    auto draw_ui = [&]() {
        std::lock_guard<std::mutex> lock(ui_state.mtx);

        // Clear background
        XSetForeground(dpy, gc, col_bg);
        XFillRectangle(dpy, win, gc, 0, 0, win_w, win_h);

        // Header Title
        XSetForeground(dpy, gc, col_accent);
        std::string title = "ARCH LINUX PEER-TO-PEER FILE & FOLDER SHARE";
        XDrawString(dpy, win, gc, 24, 35, title.c_str(), title.length());

        std::string host_info = "Local Host: " + FileUtil::get_local_hostname() + " | Saves to: " + FileUtil::get_downloads_dir();
        XSetForeground(dpy, gc, col_text_dim);
        XDrawString(dpy, win, gc, 24, 55, host_info.c_str(), host_info.length());

        // Mode Toggle Buttons (Client vs Sender)
        int btn_w = 150;
        int btn_h = 36;
        int btn1_x = 24;
        int btn2_x = 184;
        int btn_y = 70;

        // Button 1: Client (Receiver)
        bool is_client = (ui_state.mode == AppMode::CLIENT);
        XSetForeground(dpy, gc, is_client ? col_btn_active : col_btn_inactive);
        XFillRectangle(dpy, win, gc, btn1_x, btn_y, btn_w, btn_h);
        XSetForeground(dpy, gc, is_client ? col_accent_green : col_card_border);
        XDrawRectangle(dpy, win, gc, btn1_x, btn_y, btn_w, btn_h);

        XSetForeground(dpy, gc, is_client ? col_accent_green : col_text_dim);
        std::string btn1_text = "[ Client / Receiver ]";
        XDrawString(dpy, win, gc, btn1_x + 14, btn_y + 22, btn1_text.c_str(), btn1_text.length());

        // Button 2: Sender
        bool is_sender = (ui_state.mode == AppMode::SENDER);
        XSetForeground(dpy, gc, is_sender ? col_btn_active : col_btn_inactive);
        XFillRectangle(dpy, win, gc, btn2_x, btn_y, btn_w, btn_h);
        XSetForeground(dpy, gc, is_sender ? col_accent : col_card_border);
        XDrawRectangle(dpy, win, gc, btn2_x, btn_y, btn_w, btn_h);

        XSetForeground(dpy, gc, is_sender ? col_accent : col_text_dim);
        std::string btn2_text = "[ Sender ]";
        XDrawString(dpy, win, gc, btn2_x + 42, btn_y + 22, btn2_text.c_str(), btn2_text.length());

        // Card Container Area
        int card_x = 24;
        int card_y = 120;
        int card_w = win_w - 48;
        int card_h = 320;

        XSetForeground(dpy, gc, col_card);
        XFillRectangle(dpy, win, gc, card_x, card_y, card_w, card_h);
        XSetForeground(dpy, gc, col_card_border);
        XDrawRectangle(dpy, win, gc, card_x, card_y, card_w, card_h);

        if (ui_state.mode == AppMode::CLIENT) {
            // CLIENT VIEW
            XSetForeground(dpy, gc, col_accent_green);
            std::string c_head = "CLIENT / RECEIVER MODE ACTIVE";
            XDrawString(dpy, win, gc, card_x + 20, card_y + 40, c_head.c_str(), c_head.length());

            XSetForeground(dpy, gc, col_text);
            std::string c_desc1 = "• Automatically broadcasting presence to other Arch Linux desktops on LAN.";
            std::string c_desc2 = "• Incoming files and folders will be saved to your Downloads directory:";
            std::string c_path  = "  " + FileUtil::get_downloads_dir();
            std::string c_desc3 = "• Full directory hierarchy, permissions, and timestamps are preserved.";

            XDrawString(dpy, win, gc, card_x + 20, card_y + 80, c_desc1.c_str(), c_desc1.length());
            XDrawString(dpy, win, gc, card_x + 20, card_y + 110, c_desc2.c_str(), c_desc2.length());
            XSetForeground(dpy, gc, col_accent);
            XDrawString(dpy, win, gc, card_x + 20, card_y + 135, c_path.c_str(), c_path.length());
            XSetForeground(dpy, gc, col_text);
            XDrawString(dpy, win, gc, card_x + 20, card_y + 170, c_desc3.c_str(), c_desc3.length());

            // Big status banner
            int stat_box_y = card_y + 200;
            XSetForeground(dpy, gc, col_btn_inactive);
            XFillRectangle(dpy, win, gc, card_x + 20, stat_box_y, card_w - 40, 80);
            XSetForeground(dpy, gc, col_card_border);
            XDrawRectangle(dpy, win, gc, card_x + 20, stat_box_y, card_w - 40, 80);

            XSetForeground(dpy, gc, col_accent_green);
            std::string stat_title = "Receiver Status: Listening on TCP 50001 & UDP 50000";
            XDrawString(dpy, win, gc, card_x + 35, stat_box_y + 30, stat_title.c_str(), stat_title.length());

            XSetForeground(dpy, gc, col_text);
            std::string stat_txt = ui_state.is_transferring ? ui_state.current_transfer_item : ui_state.status_msg;
            XDrawString(dpy, win, gc, card_x + 35, stat_box_y + 55, stat_txt.c_str(), stat_txt.length());

        } else {
            // SENDER VIEW
            // Left Column: Peer list (Discovered systems)
            int col1_w = 260;
            XSetForeground(dpy, gc, col_text);
            std::string p_title = "Discovered Network Peers (" + std::to_string(ui_state.peers.size()) + "):";
            XDrawString(dpy, win, gc, card_x + 15, card_y + 25, p_title.c_str(), p_title.length());

            int peer_box_y = card_y + 35;
            int peer_box_h = card_h - 50;
            XSetForeground(dpy, gc, col_bg);
            XFillRectangle(dpy, win, gc, card_x + 15, peer_box_y, col1_w, peer_box_h);
            XSetForeground(dpy, gc, col_card_border);
            XDrawRectangle(dpy, win, gc, card_x + 15, peer_box_y, col1_w, peer_box_h);

            if (ui_state.peers.empty()) {
                XSetForeground(dpy, gc, col_text_dim);
                std::string no_peer = "Scanning network...";
                XDrawString(dpy, win, gc, card_x + 30, peer_box_y + 40, no_peer.c_str(), no_peer.length());
            } else {
                for (size_t i = 0; i < ui_state.peers.size() && i < 7; ++i) {
                    int item_y = peer_box_y + 5 + i * 36;
                    bool is_sel = (static_cast<int>(i) == ui_state.selected_peer_idx);

                    if (is_sel) {
                        XSetForeground(dpy, gc, col_btn_active);
                        XFillRectangle(dpy, win, gc, card_x + 18, item_y, col1_w - 6, 32);
                        XSetForeground(dpy, gc, col_accent);
                        XDrawRectangle(dpy, win, gc, card_x + 18, item_y, col1_w - 6, 32);
                    }

                    XSetForeground(dpy, gc, is_sel ? col_accent : col_text);
                    std::string p_name = ui_state.peers[i].hostname;
                    XDrawString(dpy, win, gc, card_x + 25, item_y + 16, p_name.c_str(), p_name.length());

                    XSetForeground(dpy, gc, is_sel ? col_text : col_text_dim);
                    std::string p_ip = ui_state.peers[i].ip + ":" + std::to_string(ui_state.peers[i].port);
                    XDrawString(dpy, win, gc, card_x + 25, item_y + 28, p_ip.c_str(), p_ip.length());
                }
            }

            // Right Column: Drag & Drop Dropzone
            int drop_x = card_x + col1_w + 30;
            int drop_y = card_y + 35;
            int drop_w = card_w - col1_w - 45;
            int drop_h = card_h - 50;

            XSetForeground(dpy, gc, is_drag_over ? col_drop_hover : col_bg);
            XFillRectangle(dpy, win, gc, drop_x, drop_y, drop_w, drop_h);
            XSetForeground(dpy, gc, is_drag_over ? col_accent_green : col_accent_purple);
            XDrawRectangle(dpy, win, gc, drop_x, drop_y, drop_w, drop_h);

            // Inner dashed-like border
            XSetForeground(dpy, gc, is_drag_over ? col_accent_green : col_card_border);
            XDrawRectangle(dpy, win, gc, drop_x + 6, drop_y + 6, drop_w - 12, drop_h - 12);

            XSetForeground(dpy, gc, is_drag_over ? col_accent_green : col_accent_purple);
            std::string d1 = "DRAG & DROP FILES OR FOLDERS HERE";
            XDrawString(dpy, win, gc, drop_x + 25, drop_y + 80, d1.c_str(), d1.length());

            XSetForeground(dpy, gc, col_text);
            std::string d2 = "1. Select target peer from left list";
            std::string d3 = "2. Drag any file or folder from Dolphin / File Manager";
            std::string d4 = "3. Drops will recursively stream with full metadata";
            XDrawString(dpy, win, gc, drop_x + 15, drop_y + 130, d2.c_str(), d2.length());
            XDrawString(dpy, win, gc, drop_x + 15, drop_y + 155, d3.c_str(), d3.length());
            XDrawString(dpy, win, gc, drop_x + 15, drop_y + 180, d4.c_str(), d4.length());
        }

        // Bottom Progress / Status Bar
        int stat_y = win_h - 55;
        XSetForeground(dpy, gc, col_card);
        XFillRectangle(dpy, win, gc, 24, stat_y, win_w - 48, 40);
        XSetForeground(dpy, gc, col_card_border);
        XDrawRectangle(dpy, win, gc, 24, stat_y, win_w - 48, 40);

        if (ui_state.is_transferring) {
            double pct = (ui_state.total_bytes > 0) ? (static_cast<double>(ui_state.bytes_transferred) / ui_state.total_bytes) : 0.0;
            int bar_w = static_cast<int>((win_w - 52) * pct);

            XSetForeground(dpy, gc, col_accent);
            XFillRectangle(dpy, win, gc, 26, stat_y + 2, bar_w, 36);

            XSetForeground(dpy, gc, col_text);
            std::string progress_text = ui_state.current_transfer_item + " [" + FileUtil::format_bytes(ui_state.bytes_transferred) + "]";
            XDrawString(dpy, win, gc, 36, stat_y + 24, progress_text.c_str(), progress_text.length());
        } else {
            XSetForeground(dpy, gc, col_text);
            XDrawString(dpy, win, gc, 36, stat_y + 24, ui_state.status_msg.c_str(), ui_state.status_msg.length());
        }
    };

    // Helper to initiate send when files are dropped
    auto handle_dropped_paths = [&](const std::vector<std::string>& paths) {
        if (paths.empty()) return;

        std::string peer_ip;
        uint16_t peer_port = DEFAULT_TRANSFER_PORT;
        std::string peer_name;

        {
            std::lock_guard<std::mutex> lock(ui_state.mtx);
            if (ui_state.peers.empty() || ui_state.selected_peer_idx < 0 || ui_state.selected_peer_idx >= static_cast<int>(ui_state.peers.size())) {
                ui_state.status_msg = "No target peer selected! Select a peer on the left first.";
                return;
            }
            peer_ip = ui_state.peers[ui_state.selected_peer_idx].ip;
            peer_port = ui_state.peers[ui_state.selected_peer_idx].port;
            peer_name = ui_state.peers[ui_state.selected_peer_idx].hostname;
            ui_state.status_msg = "Connecting to " + peer_name + "...";
            ui_state.is_transferring = true;
        }

        draw_ui();
        XFlush(dpy);

        std::thread([&net, &ui_state, peer_ip, peer_port, peer_name, paths, progress_callback]() {
            net.send_paths(peer_ip, peer_port, paths, progress_callback);
        }).detach();
    };

    // Helper to decode dropped text/uri-list
    auto parse_uri_list = [](const std::string& uri_data) -> std::vector<std::string> {
        std::vector<std::string> result;
        std::istringstream stream(uri_data);
        std::string line;
        while (std::getline(stream, line)) {
            // Strip CR
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;

            // Check if file:// URI
            if (line.rfind("file://", 0) == 0) {
                std::string path = line.substr(7);
                // On some systems file://localhost/path or file:///path
                if (path.rfind("localhost", 0) == 0) {
                    path = path.substr(9);
                }
                path = FileUtil::url_decode(path);
                result.push_back(path);
            } else if (line[0] == '/') {
                result.push_back(line);
            }
        }
        return result;
    };

    bool running = true;

    while (running) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        switch (ev.type) {
            case Expose: {
                draw_ui();
                break;
            }
            case ConfigureNotify: {
                win_w = ev.xconfigure.width;
                win_h = ev.xconfigure.height;
                draw_ui();
                break;
            }
            case ButtonPress: {
                int x = ev.xbutton.x;
                int y = ev.xbutton.y;

                // Mode Button Clicks
                int btn1_x = 24;
                int btn2_x = 184;
                int btn_y = 70;
                int btn_w = 150;
                int btn_h = 36;

                if (y >= btn_y && y <= btn_y + btn_h) {
                    if (x >= btn1_x && x <= btn1_x + btn_w) {
                        // Clicked Client mode
                        {
                            std::lock_guard<std::mutex> lock(ui_state.mtx);
                            ui_state.mode = AppMode::CLIENT;
                            ui_state.status_msg = "Client mode: Listening for incoming files in Downloads.";
                        }
                        net.set_is_client(true);
                        draw_ui();
                    } else if (x >= btn2_x && x <= btn2_x + btn_w) {
                        // Clicked Sender mode
                        {
                            std::lock_guard<std::mutex> lock(ui_state.mtx);
                            ui_state.mode = AppMode::SENDER;
                            ui_state.status_msg = "Sender mode: Select a peer and drag & drop files.";
                        }
                        net.set_is_client(false);
                        draw_ui();
                    }
                }

                // Peer List Selection Click
                if (ui_state.mode == AppMode::SENDER) {
                    int card_x = 24;
                    int card_y = 120;
                    int col1_w = 260;
                    int peer_box_y = card_y + 35;

                    if (x >= card_x + 15 && x <= card_x + 15 + col1_w && y >= peer_box_y && y <= peer_box_y + 260) {
                        int clicked_idx = (y - peer_box_y - 5) / 36;
                        std::lock_guard<std::mutex> lock(ui_state.mtx);
                        if (clicked_idx >= 0 && clicked_idx < static_cast<int>(ui_state.peers.size())) {
                            ui_state.selected_peer_idx = clicked_idx;
                            ui_state.status_msg = "Selected peer: " + ui_state.peers[clicked_idx].hostname;
                            draw_ui();
                        }
                    }
                }
                break;
            }
            case ClientMessage: {
                if (ev.xclient.message_type == wm_delete) {
                    running = false;
                } else if (ev.xclient.message_type == xdnd_enter) {
                    drag_source_win = ev.xclient.data.l[0];
                    is_drag_over = true;
                    draw_ui();
                } else if (ev.xclient.message_type == xdnd_position) {
                    Window src = ev.xclient.data.l[0];
                    Time time = ev.xclient.data.l[2];

                    XClientMessageEvent reply{};
                    reply.type = ClientMessage;
                    reply.window = src;
                    reply.message_type = xdnd_status;
                    reply.format = 32;
                    reply.data.l[0] = win;
                    reply.data.l[1] = 1; // Accept drop
                    reply.data.l[2] = 0;
                    reply.data.l[3] = 0;
                    reply.data.l[4] = xdnd_action_copy; // Copy action

                    XSendEvent(dpy, src, False, 0, (XEvent*)&reply);
                    XFlush(dpy);
                } else if (ev.xclient.message_type == xdnd_drop) {
                    Window src = ev.xclient.data.l[0];
                    Time time = ev.xclient.data.l[2];

                    // Request drop data via Selection
                    XConvertSelection(dpy, xdnd_selection, text_uri_list, prop_xdnd_selection, win, time);

                    is_drag_over = false;
                    draw_ui();
                }
                break;
            }
            case SelectionNotify: {
                if (ev.xselection.property != None) {
                    Atom type;
                    int format;
                    unsigned long nitems, bytes_after;
                    unsigned char* data = nullptr;

                    XGetWindowProperty(
                        dpy, win, ev.xselection.property, 0, 65536, True, AnyPropertyType,
                        &type, &format, &nitems, &bytes_after, &data
                    );

                    if (data) {
                        std::string uri_list(reinterpret_cast<char*>(data), nitems);
                        XFree(data);

                        auto paths = parse_uri_list(uri_list);
                        if (!paths.empty()) {
                            // If dropped while in Client mode, switch to Sender automatically so user can send
                            {
                                std::lock_guard<std::mutex> lock(ui_state.mtx);
                                ui_state.mode = AppMode::SENDER;
                            }
                            handle_dropped_paths(paths);
                        }
                    }

                    // Send XdndFinished
                    if (drag_source_win != None) {
                        XClientMessageEvent finish{};
                        finish.type = ClientMessage;
                        finish.window = drag_source_win;
                        finish.message_type = xdnd_finished;
                        finish.format = 32;
                        finish.data.l[0] = win;
                        finish.data.l[1] = 1; // Success
                        finish.data.l[2] = xdnd_action_copy;

                        XSendEvent(dpy, drag_source_win, False, 0, (XEvent*)&finish);
                        XFlush(dpy);
                        drag_source_win = None;
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    if (font) XFreeFont(dpy, font);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    net.stop_server();
    net.stop_discovery();

    return 0;
}
