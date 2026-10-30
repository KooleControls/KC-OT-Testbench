#include <stdio.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "BoardContext.h"
#include "StruxContext.h"
#include "AppContext.h"

static const char* TAG = "main";

// Three layers, bottom to top, and the dependencies run one way only:
//
//   BoardContext   the hardware. Knows nothing above it.
//   StruxContext   the framework. Knows the board? No — see StruxProvider.h.
//   AppContext     this product. Knows both.
//
// Each layer is the same pair: a CONTEXT that owns the layer's instances, and a PROVIDER
// that says what the layer above may reach for — BoardProvider, StruxProvider,
// AppProvider. A manager therefore takes exactly one reference, its own layer's provider,
// and finds everything through it.
//
// This file is deliberately four calls long. The ORDER WITHIN each layer lives in that
// layer's context, so a fork pulling a new Strux manager gets its position along with it
// instead of having to be told.
BoardContext board;
StruxContext strux;
AppContext application{ board, strux };

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting up...");

    board.Init();         // hardware first: it depends on nothing
    strux.Init();         // then the framework the application registers into
    application.Init();   // then the product

    // Mark firmware as valid so the bootloader doesn't roll back on next reboot
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "All layers initialized, firmware confirmed valid");
}
