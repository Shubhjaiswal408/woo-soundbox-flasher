// ==========================================================
// secrets.h -- optional defaults for the setup form.
// Leave these empty for a public build: every box is then set up
// through its own Wi-Fi setup page (Woo-Soundbox / soundbox).
// Fill them in only for your own boards, to pre-fill the form.
// The API key should be READ-ONLY (WooCommerce > Settings >
// Advanced > REST API > Add key > Permissions: Read).
// ==========================================================
#pragma once

#define WIFI_SSID       ""
#define WIFI_PASSWORD   ""

// Store address WITH the trailing slash, e.g. "https://example.com/"
#define STORE_URL       ""

#define WC_KEY          ""      // ck_...
#define WC_SECRET       ""      // cs_...
