/*
 * Copyright © 2014 Bart Massey
 * [This program is licensed under the "MIT License"]
 * Please see the file COPYING in the source
 * distribution of this software for license terms.
 */ 
   
/* ASCII xdu replacement with reasonable performance. */
 
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* For command line variables */
#include <getopt.h>

/* For GUI - with backend */
#include <cairo.h>
#include <gtk/gtk.h>

/* Number of entries to consider "largest small". */
#define DU_INIT_ENTRIES_SIZE (128 * 1024)

/* For portability. */
#define DU_PATH_MAX 4096
#define DU_COMPONENTS_MAX DU_PATH_MAX

/* Number of spaces of indent per level. */
#define N_INDENT 2

struct entry {
    uint64_t size;		
    uint32_t n_components;	// # of components that makeup this entry
    char *path;   		// for later free 
    char **components;		// The actual components of this entry
    uint32_t depth;		// The depth of this entry in the directory tree
    uint32_t n_children;	// # of children directories at this entry level
    struct entry **children;	// Children entries of this entry
};

int n_entries = 0;
int max_entries = 0;
struct entry *entries = 0;

void read_entries(FILE *f) {
    int line_number = 0;
    /* Size field is at most 2**64 bytes, but in kB.
     * Thus max digits in 64-bit size is
     *   ceil(log10(2**64 / 1024)) = 17
     * Buffer is size field + separator + "./" + path + newline.
     */
    int du_buffer_length = 17 + 1 + (2 + DU_PATH_MAX) + 1;
    /* Loop reading lines from the du file and processing them. */
    while (1) {
        /* Get a buffer for the line data. */
        char *path = malloc(du_buffer_length);
        if (!path) {
            perror("malloc");
            exit(1);
        }
        /* Read the next line. */
        path[du_buffer_length - 1] = '\0';
        errno = 0;
        char *result = fgets(path, du_buffer_length, f);
        if (!result) {
            if (errno != 0) {
                perror("fgets");
                exit(1);
            }
            free(path);
            entries = realloc(entries, n_entries * sizeof(entries[0]));
            if (!entries) {
                perror("realloc");
                exit(1);
            }
            return;
        }
        line_number++;
        if (path[du_buffer_length - 1] != '\0') {
            if (path[du_buffer_length - 1] == '\n')
                path[du_buffer_length - 1] = '\0';
            fprintf(stderr, "line %d: buffer overrun\n", line_number);
            exit(1);
        }
        /*
         * Don't leak a ton of data on each path. The size
         * field and separator and newline are still leaked.
         * C'est la vie.
         */
        path = realloc(path, strlen(path) + 1);
        if (!path) {
            perror("realloc");
            exit(1);
        }
        /* Allocate a new entry for the line. */
        while (n_entries >= max_entries) {
            if (max_entries == 0)
                max_entries = DU_INIT_ENTRIES_SIZE;
            else
                max_entries *= 2;
            entries = realloc(entries, max_entries * sizeof(entries[0]));
            if (!entries) {
                perror("realloc");
                exit(1);
            }
        }
        struct entry *entry = &entries[n_entries++];
        entry->path = path;
        entry->n_children = 0;
        entry->children = 0;
        /* Start to parse the line. */
        char *index = path;
        while (isdigit(*index))
            index++;
        if (index == path || (*index != ' ' && *index != '\t')) {
            fprintf(stderr, "line %d: buffer format error\n", line_number);
            exit(1);
        }
        /* Parse the size field. */
        *index++ = '\0';
        int n_scanned = sscanf(path, "%" PRIu64, &entry->size);  //Should be: PRIu64
        if (n_scanned != 1) {
            fprintf(stderr, "line %d: size parse failure\n", line_number);
            exit(1);
        }
        /*
         * Parse the path. Note that we don't skip extra separator
         * chars, on the off chance that there's a leading path that
         * starts with a whitespace character.
         */
        entry->components =
            malloc(DU_COMPONENTS_MAX * sizeof(entry->components[0]));
        if (!entry->components) {
            perror("malloc");
            exit(1);
        }
        entry->components[0] = index;
        entry->n_components = 1;
        while (1) {
            if (*index == '\n' || *index == '\0') {
                *index = '\0';
                break;
            }
            else if (*index == '/') {
                *index++ = '\0';
                entry->components[entry->n_components++] = index;
                assert(entry->n_components < DU_COMPONENTS_MAX);
            }
            else {
                index++;
            }
        }
        /* Don't leak a ton of data on each entry. */
        entry->components =
            realloc(entry->components,
                    entry->n_components * sizeof(entry->components[0]));
        if (!entry->components) {
            perror("realloc");
            exit(1);
        }
    }
    assert(0);
}

/* Component length of initial prefix. */
uint32_t base_depth = 0;

/*
 * Priorities for sort:
 *   (1) Prefixes before path extensions.
 *   (2) Ascending alphabetical order.
 */
int compare_entries(const void *p1, const void * p2) {
    const struct entry *e1 = p1;
    const struct entry *e2 = p2;
    int n1 = e1->n_components;
    int n2 = e2->n_components;
    for (int i = 0; i < n1 && i < n2; i++) {
        int q = strcmp(e1->components[i], e2->components[i]);
        if (q != 0)
            return q;
    }
    if (n1 != n2)
        return (n1 - n2);
    assert(0);
}

/* Because unsigned. This should get inlined. */
int compare_sizes(uint32_t s1, uint32_t s2) {
    if (s1 < s2)
        return -1;
    if (s1 > s2)
        return 1;
    return 0;
}

/*
 * Priorities for sort:
 *   (1) Descending entry size.
 *   (2) Ascending alphabetical order.
 */
int compare_subtrees(const void *p1, const void * p2) {
    struct entry * const *e1 = p1;
    struct entry * const *e2 = p2;
    int s1 = (*e1)->size;
    int s2 = (*e2)->size;
    int q = compare_sizes(s2, s1);
    if (q != 0)
        return q;
    assert((*e1)->depth == (*e2)->depth);
    int depth = (*e1)->depth;
    q = strcmp((*e1)->components[depth + base_depth - 1],
               (*e2)->components[depth + base_depth - 1]);
    if (q != 0)
        return q;
    assert(0);
}

/*
 * Build a tree in the entry structure. This implementation
 * utilizes post-order traversal and takes advantage of the
 * existing du sorted output - assumes user wants du output
 */
 
void build_tree_postorder(uint32_t start, uint32_t end, uint32_t depth) {
    
    /* Set up for calculation */
    struct entry *e = &entries[start];
    uint32_t offset = depth - 1;

    e->depth = depth;

    /* Fill direct children and build subtree */
    int i = start;

    while(i < end) {
        
        int j = i + 1;

        entries[j].depth = depth - 1;
        entries[j].n_children = 0;
        offset = entries[i].n_components - 1;

        while(j < end && entries[j].n_components < offset - 1 &&
              strcmp(entries[i].components[offset], 
                     entries[j].components[offset]))
        {
            entries[j].n_children++;
            j++;
        }

        if(j > i + 1)
            build_tree_postorder(i, j, entries[j].n_components - 1);
   
        entries[j].children = malloc(entries[j].n_children * sizeof(entries[j].children[0]));
    
        int n_children = 0;
        for(int k = j - 1; k > i; k--)
            if(entries[k].n_components == offset + 1)
                entries[j].children[n_children++] = &entries[k];

        i = j;

        assert(n_children == entries[j].n_children);
    }
}

/*
 * Build a tree in the entry structure. The three-pass design
 * is for monotonic malloc() usage, because efficiency.
 */
void build_tree_preorder(uint32_t start, uint32_t end, uint32_t depth) {
    
    /* Set up for calculation. */
    struct entry *e = &entries[start];
    uint32_t offset = depth + base_depth;
    if (e->n_components != offset) {
        fprintf(stderr, "index %d: unexpected entry\n", start + 1);
        exit(1);
    }
    e->depth = depth;

    /* Pass 1: Count and allocate direct children. */
    for (int i = start + 1; i < end; i++)
        if (entries[i].n_components == offset + 1)
            e->n_children++;
    e->children = malloc(e->n_children * sizeof(e->children[0]));
    if (!e->children) {
        perror("malloc");
        exit(1);
    }

    /* Pass 2: Fill direct children and build subtrees. */
    int n_children = 0;
    int i = start + 1;
    while (i < end) {
        if (entries[i].n_components != offset + 1) {
            fprintf(stderr, "index %d: missing entry\n", i + 1);
            exit(1);
        }
        e->children[n_children++] = &entries[i];
        entries[i].depth = depth + 1;
        int j = i + 1;
        /* Walk to end of subtree. */
        while (j < end && entries[j].n_components > offset + 1 &&
               !strcmp(entries[i].components[offset],
                       entries[j].components[offset]))
            j++;
        /* If subtree is found, build it. */
        if (j > i + 1)
            build_tree_preorder(i, j, depth + 1);
        i = j;
    }
    assert(n_children == e->n_children);
    /* Pass 3: Sort the children. Should this be here or in display? */
    qsort(e->children, e->n_children, sizeof(e->children[0]),
          compare_subtrees);
}

void indent(uint32_t depth) {
    for (uint64_t i = 0; i < N_INDENT * depth; i++)
        putchar(' ');
}

void show_entries(struct entry *e) {
    uint32_t depth = e->depth;
    if (depth == 0) {
        printf("%s", e->components[0]);
        for (uint32_t i = 1; i < base_depth; i++)
            printf("/%s", e->components[i]);
        printf(" %"PRIu64 "\n", e->size);
    }
    else {
        indent(depth);
        printf("%s %"PRIu64"\n",
               e->components[e->n_components - 1], e->size);
    }
    for (uint32_t i = 0; i < e->n_children; i++)
        show_entries(e->children[i]);
}

void showEntriesNew(struct entry e[], int n) {
    uint32_t depth = 0;
    uint32_t offset = 0;

    for(uint32_t i = 0; i < n; i++)
    {
	depth = e[i].depth;
	indent(depth);
        offset = e[i].n_components - 1;

	printf("%s %"PRIu64"\n", e[i].components[offset], e[i].size);
    } 
}

void status(char *msg) {
    static int pass = 1;
    fprintf(stderr, "(%d) %s\n", pass++, msg);
} 

/*
 *  Helper/testing function for displaying a simplified order
 *  that entries are currently in - formatted for directory
 *  view. Includes information about the size.
 */ 
void dispEntries(struct entry e[], int n) {
    printf("Simple Entries\n# of Entries: %d\n\n", n);

    for(int i = 0; i < n; i++) {
        if(e[i].n_components) {
            for(int j = 0; j < e[i].n_components; j++) {
                printf("%s/", e[i].components[j]);
                printf(" ,%" PRIu64 "\n", e[i].size);
            }
        }
    }
}

static void draw_node(cairo_t *cr, struct entry *e, int x, int y, int width, int height) {

    /* Length of 2**64 - 1, +1 for null */
    char sizeStr[21];

    /* Center the text in this rectangle */
    int txtX = x + width / 2; 
    int txtY = y + height / 2;

    /* Copy uint64_t into char buffer */
    sprintf(sizeStr, "%" PRIu64, e->size);

    /* Draw the rectangle container */
    cairo_rectangle(cr, x, y, width, height);
    cairo_stroke(cr);

    /* Draw the label */
    cairo_move_to(cr, txtX, txtY);
    cairo_show_text(cr, e->components[e->n_components - 1]);
    cairo_show_text(cr, " (");
    cairo_show_text(cr, sizeStr);
    cairo_show_text(cr, ")");
}

static void draw_nodes(cairo_t *cr, struct entry *e, int recW, 
                       int recH, double winWidth, double winHeight, float pSize)
{
    uint32_t depth = e->depth;
    float mod = 1;
    int width = winWidth;
    int height = winHeight;
    float parSize = pSize;

    /* Computing the root node initializes the rest of this algorithm */
    if(depth == 0) {
        width = 100;
        parSize = e->size;
        height = winHeight;
        draw_node(cr, e, recW, recH, width, height);
    }
    else {
        mod = e->size / parSize;
        height = winHeight * mod;
        draw_node(cr, e, recW, recH, width, height);
    }

    recW += width;
    int tempHeight = 0;

    /* Start drawing children */
    for(int i = 0; i < e->n_children; i++) {
        draw_nodes(cr, e->children[i], recW, recH, width, height, parSize);
        
        /* Height Coordinate of next child */
        mod = e->children[i]->size / parSize;
        tempHeight = winHeight * mod;
        recH += tempHeight;
    }
}

/* Perform the actual drawing of the entries */
static void do_drawing(GtkWidget *widget, cairo_t *cr) {

    /* How much space was the window actually allocated? */
    GtkAllocation *allocation = g_new0 (GtkAllocation, 1);
    gtk_widget_get_allocation(GTK_WIDGET(widget), allocation);

    /* Make sure that cairo is aware of the dimensions of the drawing surface */
    double winWidth = allocation->width;
    double winHeight = allocation->height;

    /* Allocation no longer needed */
    g_free(allocation);
   
    /* Set cairo drawing variables */
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_select_font_face(cr, "Helvetica", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    cairo_set_line_width(cr, 1);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);

    /* Keep track of parent size to properly scale children */
    float pSize = (float)entries[0].size;

    /* draw the nodes, starting with the root */
    draw_nodes(cr, &entries[0], 0, 0, winWidth, winHeight, pSize);     
}

/* Call up the cairo functionality */
static gboolean on_draw_event(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
   
    do_drawing(widget, cr);

    return FALSE;
}

/* Initialize the window, drawing surface, and functionality */
int gui(int argv, char **argc) {

    GtkWidget *window;
    GtkWidget *darea;

    /* Initialize GTK, the window, and the drawing surface */
    gtk_init(&argv, &argc); 
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    darea = gtk_drawing_area_new();

    /* Put the drawing surface 'inside' the window */
    gtk_container_add(GTK_CONTAINER(window), darea);

    /* Functionality handling - drawing and exiting */
    g_signal_connect(G_OBJECT(darea), "draw", G_CALLBACK(on_draw_event), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    /* Default window settings */
    gtk_window_set_title(GTK_WINDOW(window), "Duvis");
    gtk_window_set_default_size(GTK_WINDOW(window), 600, 480);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

    /* Display the window */
    gtk_widget_show_all(window);
    gtk_main();

    return (0);
}

int main(int argc, char **argv) {

    int c;
    int pflag = 0, gflag = 0;

    while((c = getopt(argc, argv, "pg")) != -1)
    {
	switch(c)
	{
	    case 'p':	// Enable pre-order sorting
		pflag = 1;
		break;
	    case 'g':	// Enable GUI
		gflag = 1;
		break;
	    case '?':	// Error handling
	        fprintf(stderr, "Unknown option -%c\n", optopt);
		abort();
	    default:	// Something really weird happened
		abort();
	}
    }

    // Read in data from du
    status("Parsing du file.");
    read_entries(stdin);

    if (n_entries == 0)
	return 0;

    // default: post order
    if(pflag == 0)
    {
	status("Building tree: Post-Order.");
  
        base_depth = entries[n_entries - 1].n_components; 
	build_tree_postorder(0, n_entries - 1, entries[0].n_components - 1);

	status("Rendering tree.");
	// display ascii or gui
	if(gflag == 0)
	{
	    showEntriesNew(entries, n_entries);
	}
	else if(gflag == 1)
	{
	    gui(argc, argv);
	}
    }
    // pre order
    else if(pflag == 1)
    {
	status("Sorting entries.");
	qsort(entries, n_entries, sizeof(entries[0]), compare_entries);

	status("Building tree: Pre-Order.");
	if(entries[0].n_components == 0)
        {
	    fprintf(stderr, "Mysterious zero-length entry in table.\n");
	    exit(1);
        }

	base_depth = entries[0].n_components;
	build_tree_preorder(0, n_entries, 0);

	status("Rendering tree.");
	// display ascii or gui
	if(gflag == 0)
	{
	    show_entries(&entries[0]);	
	}
	else if(gflag == 1)
	{
	    gui(argc, argv);
	}
    }
    
    return(0); 
}
