// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * menu_defs.c
 * The root of the overlay menu.
 *
 * The order is the receiver's signal chain -- Input, Decoder, Trunking,
 * Encryption, Audio, Recording -- then what you look at, then housekeeping.
 * It matches the reading order of the main screen, and it gives every new
 * item one obvious home: put it where its signal is.
 */

#include <dsd-neo/ui/menu_defs.h>

// All NcMenuItem arrays are declared in menu_items.h
#include "dsd-neo/ui/menu_core.h"
#include "menu_items.h"

void
ui_menu_get_main_items(const NcMenuItem** out_items, size_t* out_n, UiCtx* ctx) {
    (void)ctx; // context used by callbacks; arrays are static so safe to expose
    static int inited = 0;
    static NcMenuItem items[11];
    if (!inited) {
        items[0] = (NcMenuItem){.id = "main.input",
                                .label = "Input",
                                .help = "Signal source, input level, RTL-SDR tuning.",
                                .submenu = INPUT_MENU_ITEMS,
                                .submenu_len = INPUT_MENU_ITEMS_LEN};
        items[1] = (NcMenuItem){.id = "main.decoder",
                                .label = "Decoder",
                                .help = "Protocol mode, modulation, filters, DMR/TDMA.",
                                .submenu = DECODER_MENU_ITEMS,
                                .submenu_len = DECODER_MENU_ITEMS_LEN};
        items[2] = (NcMenuItem){.id = "main.trunking",
                                .label = "Trunking",
                                .help = "Trunking, scanning, follow rules, imports, P25, rig control.",
                                .submenu = TRUNK_MENU_ITEMS,
                                .submenu_len = TRUNK_MENU_ITEMS_LEN};
        items[3] = (NcMenuItem){.id = "main.encryption",
                                .label = "Encryption",
                                .help = "Keys, key import, keystreams, encrypted-call policy.",
                                .submenu = ENC_MENU_ITEMS,
                                .submenu_len = ENC_MENU_ITEMS_LEN};
        items[4] = (NcMenuItem){.id = "main.audio",
                                .label = "Audio",
                                .help = "Output sink, mute, gains, monitor, tone shaping, call alerts.",
                                .submenu = AUDIO_MENU_ITEMS,
                                .submenu_len = AUDIO_MENU_ITEMS_LEN};
        items[5] = (NcMenuItem){.id = "main.recording",
                                .label = "Recording & logs",
                                .help = "Symbol capture, WAV, event log, LRRP, DSP dumps.",
                                .submenu = REC_MENU_ITEMS,
                                .submenu_len = REC_MENU_ITEMS_LEN};
        items[6] = (NcMenuItem){.id = "main.display",
                                .label = "Display",
                                .help = "Compact view, on-screen sections, visualizers, event history.",
                                .submenu = DISPLAY_MENU_ITEMS,
                                .submenu_len = DISPLAY_MENU_ITEMS_LEN};
        items[7] = (NcMenuItem){.id = "main.config",
                                .label = "Config",
                                .help = "Load and save settings and profiles.",
                                .submenu = CONFIG_MENU_ITEMS,
                                .submenu_len = CONFIG_MENU_ITEMS_LEN};
        items[8] = (NcMenuItem){.id = "main.advanced",
                                .label = "Advanced",
                                .help = "Scheduling, threads, FTZ/DAZ, diagnostics, environment.",
                                .submenu = ADV_MENU_ITEMS,
                                .submenu_len = ADV_MENU_ITEMS_LEN};
        items[9] = (NcMenuItem){.id = "main.sep", .kind = NC_ITEM_SEPARATOR};
        // The rule above is what the eye reads; `no_jump` is what stops the
        // highlight. Without it End lands here and the next Enter quits a running
        // decode -- two keystrokes, no confirmation. Arrows, Page Down and the 'q'
        // hotkey still reach it; only the jump to the end of the list does not.
        items[10] = (NcMenuItem){.id = "exit",
                                 .label = "Quit DSD-neo",
                                 .help = "Quit the application.",
                                 .hotkey = "q",
                                 .on_select = act_exit,
                                 .no_jump = true};
        inited = 1;
    }
    if (out_items) {
        *out_items = items;
    }
    if (out_n) {
        *out_n = sizeof items / sizeof items[0];
    }
}
