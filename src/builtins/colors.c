/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/colors.c - Display terminal color palette and reference
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include <stdio.h>
#include <math.h>

/* ---- HSV to RGB conversion for true-color gradient ---------------------- */

static void hsv_to_rgb(double h, double s, double v,
                       int *r, int *g, int *b) {
    double c = v * s;
    double x = c * (1.0 - fabs(fmod(h / 60.0, 2.0) - 1.0));
    double m = v - c;

    double rp, gp, bp;
    if      (h < 60)  { rp = c; gp = x; bp = 0; }
    else if (h < 120) { rp = x; gp = c; bp = 0; }
    else if (h < 180) { rp = 0; gp = c; bp = x; }
    else if (h < 240) { rp = 0; gp = x; bp = c; }
    else if (h < 300) { rp = x; gp = 0; bp = c; }
    else              { rp = c; gp = 0; bp = x; }

    *r = (int)((rp + m) * 255.0);
    *g = (int)((gp + m) * 255.0);
    *b = (int)((bp + m) * 255.0);
}

/* ---- Standard 16 colors section ----------------------------------------- */

static void print_standard_colors(void) {
    static const char *names[] = {
        "Black",   "Red",      "Green",   "Yellow",
        "Blue",    "Magenta",  "Cyan",    "White",
        "BrBlack", "BrRed",    "BrGreen", "BrYellow",
        "BrBlue",  "BrMagenta","BrCyan",  "BrWhite"
    };

    printf("\033[1mStandard Colors (0-7):\033[0m\n");
    for (int i = 0; i < 8; i++) {
        printf("  \033[48;5;%dm   \033[0m", i);
        printf(" %2d %-10s", i, names[i]);
        if (i == 3) printf("\n");
    }
    printf("\n\n");

    printf("\033[1mBright Colors (8-15):\033[0m\n");
    for (int i = 8; i < 16; i++) {
        printf("  \033[48;5;%dm   \033[0m", i);
        printf(" %2d %-10s", i, names[i]);
        if (i == 11) printf("\n");
    }
    printf("\n\n");
}

/* ---- 256-color palette section ------------------------------------------ */

static void print_256_colors(void) {
    printf("\033[1m216 Color Cube (16-231):\033[0m\n");

    /*
     * Colors 16-231 form a 6x6x6 RGB cube.
     * Index = 16 + 36*r + 6*g + b  (r,g,b in 0..5)
     *
     * Display as 6 blocks (one per red level), each block being 6 rows
     * (green) x 6 cols (blue).  We'll show all 6 red blocks side by side
     * for compactness: 6 rows, each row has 36 colored blocks.
     */
    for (int g = 0; g < 6; g++) {
        printf("  ");
        for (int r = 0; r < 6; r++) {
            for (int b = 0; b < 6; b++) {
                int idx = 16 + 36 * r + 6 * g + b;
                printf("\033[48;5;%dm  \033[0m", idx);
            }
            if (r < 5) printf(" ");
        }
        printf("\n");
    }
    printf("\n");

    printf("\033[1mGrayscale Ramp (232-255):\033[0m\n  ");
    for (int i = 232; i <= 255; i++) {
        printf("\033[48;5;%dm  \033[0m", i);
    }
    printf("\n\n");
}

/* ---- True-color rainbow gradient ---------------------------------------- */

static void print_truecolor_gradient(void) {
    printf("\033[1mTrue Color Gradient (24-bit):\033[0m\n  ");

    int width = 80;
    for (int i = 0; i < width; i++) {
        double hue = (double)i / (double)width * 360.0;
        int r, g, b;
        hsv_to_rgb(hue, 1.0, 1.0, &r, &g, &b);
        printf("\033[48;2;%d;%d;%dm \033[0m", r, g, b);
    }
    printf("\n\n");
}

/* ---- ANSI reference ----------------------------------------------------- */

static void print_reference(void) {
    printf("\033[1mANSI Color Code Reference:\033[0m\n");
    printf("  \\033[38;5;Nm      - 256-color foreground (N = 0-255)\n");
    printf("  \\033[48;5;Nm      - 256-color background (N = 0-255)\n");
    printf("  \\033[38;2;R;G;Bm  - True-color foreground (RGB 0-255)\n");
    printf("  \\033[48;2;R;G;Bm  - True-color background (RGB 0-255)\n");
    printf("  \\033[0m            - Reset all attributes\n");
    printf("  \\033[1m            - Bold\n");
    printf("  \\033[2m            - Dim\n");
    printf("  \\033[4m            - Underline\n");
}

/* ---- Main entry point --------------------------------------------------- */

/*
 * colors
 *
 * Display the terminal color palette: standard 16 colors, 256-color
 * palette, true-color gradient, and an ANSI code reference.
 */
int builtin_colors(Shell *shell, int argc, char **argv) {
    (void)shell; (void)argc; (void)argv;

    printf("\n");
    print_standard_colors();
    print_256_colors();
    print_truecolor_gradient();
    print_reference();
    printf("\n");

    return 0;
}
