
#include <stdlib.h>
#include <string.h>

#include <sys/ioctl.h>

#include "vterm.h"
#include "vterm_private.h"
#include "vterm_buffer.h"

void
vterm_buffer_alloc(vterm_t *vterm, int idx, int width, int height)
{
    vterm_desc_t    *v_desc;
    int             i;

    if(vterm == NULL) return;
    if(idx != VTERM_BUFFER_STD && idx != VTERM_BUFFER_ALT) return;

    if(width < 0 || height < 0) return;

    v_desc = &vterm->vterm_desc[idx];

    v_desc->cells =
        (vterm_cell_t**)calloc(1, (sizeof(vterm_cell_t*) * height));

    for(i = 0;i < height;i++)
    {
        v_desc->cells[i] =
            (vterm_cell_t*)calloc(1, (sizeof(vterm_cell_t) * width));
    }

    v_desc->rows = height;
    v_desc->cols = width;

    v_desc->scroll_min = 0;
    v_desc->scroll_max = height - 1;

    return;
}

void
vterm_buffer_realloc(vterm_t *vterm, int idx, int width, int height)
{
    vterm_desc_t    *v_desc;
    int             delta_y = 0;
    int             start_x = 0;
    uint16_t        i;
    uint16_t        j;

    if(vterm == NULL) return;
    if(idx != VTERM_BUFFER_STD && idx != VTERM_BUFFER_ALT) return;

    if(width == 0 || height == 0) return;

    // set the vterm description buffer selector
    v_desc = &vterm->vterm_desc[idx];

    delta_y = height - v_desc->rows;

    // realloc to accomodate the new matrix size
    v_desc->cells = (vterm_cell_t**)realloc(v_desc->cells,
        sizeof(vterm_cell_t*) * height);

    for(i = 0; i < height; i++)
    {
        // when adding new rows, we can just calloc() them.
        if((delta_y > 0) && (i > (v_desc->rows - 1)))
        {
            v_desc->cells[i] =
                (vterm_cell_t*)calloc(1, (sizeof(vterm_cell_t) * width));

            // fill new row with blanks
            for(j = 0; j < width; j++)
            {
                VCELL_SET_CHAR(v_desc->cells[i][j], ' ');
            }

            continue;
        }

        // this handles existing rows
        v_desc->cells[i] = (vterm_cell_t*)realloc(v_desc->cells[i],
            sizeof(vterm_cell_t) * width);

        // fill new cols with blanks
        start_x = v_desc->cols - 1;
        for(j = start_x; j < width; j++)
        {
            VCELL_ZERO_ALL(v_desc->cells[i][j]);
            VCELL_SET_CHAR(v_desc->cells[i][j], ' ');
        }
    }

    v_desc->rows = height;
    v_desc->cols = width;

    v_desc->scroll_max = height - 1;

    return;
}


void
vterm_buffer_dealloc(vterm_t *vterm, int idx)
{
    vterm_desc_t    *v_desc;
    int             i;

    if(vterm == NULL) return;
    if(idx != VTERM_BUFFER_STD && idx != VTERM_BUFFER_ALT) return;

    v_desc = &vterm->vterm_desc[idx];

    // prevent a double-free
    if(v_desc->cells == NULL) return;

    for(i = 0; i < v_desc->rows; i++)
    {
        free(v_desc->cells[i]);
        v_desc->cells[i] = NULL;
    }

    free(v_desc->cells);
    v_desc->cells = NULL;

    return;
}

int
vterm_buffer_set_active(vterm_t *vterm, int idx)
{
    vterm_desc_t    *v_desc = NULL;
    int             curr_idx;
    struct winsize  ws = {.ws_xpixel = 0, .ws_ypixel = 0};
    int             width;
    int             height;
    int             std_width, std_height;
    int             curr_width, curr_height;

    if(vterm == NULL) return -1;
    if(idx != VTERM_BUFFER_STD && idx != VTERM_BUFFER_ALT) return -1;

    curr_idx = vterm_buffer_get_active(vterm);

    /*
        check to see if current buffer index is the same as the
        requested one.  if so, this is a no-op.
    */
    if(idx == curr_idx) return 0;

    /*
        get current terminal size using the best methods available.  typically,
        that means using TIOCGWINSZ ioctl which is pretty portable.  an
        alternative method could be getmaxyx() which is found in most curses
        implementations.  however, that causes problems for uses of libvterm
        where the rendering apparatus is not a curses WINDOW.
    */
    ioctl(vterm->pty_fd, TIOCGWINSZ, &ws);
    height = ws.ws_row;
    width = ws.ws_col;

    // treat the standard buffer special -- it never goes away
    if(idx == VTERM_BUFFER_STD)
    {
        /*
            if the current buffer not the STD buffer, we need to handle
            a few housekeeping items and then tear down the other
            buffer before switching.
        */
        if(curr_idx != VTERM_BUFFER_STD)
        {
            // check to see if a resize happened
            std_width = vterm->vterm_desc[VTERM_BUFFER_STD].cols;
            std_height = vterm->vterm_desc[VTERM_BUFFER_STD].rows;
            curr_width = vterm->vterm_desc[curr_idx].cols;
            curr_height = vterm->vterm_desc[curr_idx].rows;

            if(std_height != curr_height || std_width != curr_width)
            {
                vterm_buffer_realloc(vterm, VTERM_BUFFER_STD,
                    curr_width, curr_height);
            }

            // run the event hook if installed
            if(vterm->event_hook != NULL)
            {
                vterm->event_hook(vterm, VTERM_HOOK_BUFFER_DEACTIVATED,
                    (void *)&curr_idx);
            }

            vterm_buffer_dealloc(vterm, curr_idx);
        }

        // restore the default color palette
        // color_cache_load_palette(vterm->color_cache, PALETTE_SAVED);
    }

    /*
        given the above conditional, this could have been an if-else.
        however, this is more readable and it should optimize
        about the same.
    */
    if(idx != VTERM_BUFFER_STD)
    {
        vterm_buffer_alloc(vterm, idx, width, height);
        v_desc = &vterm->vterm_desc[idx];

        // take a snapshot of the color palette before switching buffers
        // color_cache_save_palette(vterm->color_cache, PALETTE_SAVED);

        // copy some defaults from standard buffer
        v_desc->default_colors =
            vterm->vterm_desc[VTERM_BUFFER_STD].default_colors;

        // erase the newly created buffer
        vterm_erase(vterm, idx);
    }

    // update the vterm buffer desc index
    vterm->vterm_desc_idx = idx;

    // run the event hook if installed
    if(vterm->event_hook != NULL)
    {
        vterm->event_hook(vterm, VTERM_HOOK_BUFFER_ACTIVATED,
            (void *)&idx);
    }

    return 0;
}

int
vterm_buffer_get_active(vterm_t *vterm)
{
    if(vterm == NULL) return -1;

    return vterm->vterm_desc_idx;
}

