#include <cui/cui.h>
#include <cui/cui_app.h>

#include "katoshe.h"

static void frame(cui_ctx *ui, void *user) {
    (void)user;

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 24, .gap = 12,
               .bg = CUI_COLOR_BACKGROUND) {
        cui_heading(ui, "Katoshe", 2);
        cui_text(ui, "GitHub release asset manager");
        cui_separator(ui);
        cui_text(ui, "Skeleton only. Repositories, Assets, Run and Settings arrive in Phase 7.");
        cui_spacer(ui);
        cui_textf(ui, "version %s", katoshe_version());
    }
}

int main(void) {
    return cui_app_run(&(cui_app_desc){ .title = "Katoshe", .width = 1000, .height = 700 },
                       frame, NULL);
}
