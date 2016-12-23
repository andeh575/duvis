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

#include "duvis.h"
#include "pathmem.h"

#define IO_BUFFER_LENGTH (1024 * 1024)

int n_entries = 0;
struct entry *entries = 0;
struct entry *root_entry;
int base_depth = 0;	/* Component length of initial prefix */

static void read_entries(FILE *f, int zeroflag) {
    int max_entries = 0;
    int line_number = 0;
    
    while (1) {
        /* Get a buffer for the line data. */
        char *path = path_alloc();

        if (!path) {
            perror("malloc");
            exit(1);
        }

        /* Read the next line. */
        path[DU_BUFFER_LENGTH - 1] = '\0';
        errno = 0;

       int nchars = path_get(path, DU_BUFFER_LENGTH, f ,zeroflag);

        if (nchars == -1)
            fprintf(stderr, "line %d: path buffer overrun\n", line_number + 1);

        if (nchars == 0) {
            entries = realloc(entries, n_entries * sizeof(entries[0]));
            
            if (!entries) {
                perror("realloc");
                exit(1);
            }
            return;
        }

        line_number++;

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
    uint32_t offset = 0;

    e->depth = depth;
    e->n_children = 0;

    /* Find a subtree */
    int i = start;
    while(i < end) {
        
        int j = i + 1;
  
        offset = entries[i].n_components - 2;

        /* Go to the end of this subtree */
        while(j < end && entries[j].n_components < offset &&
                !strcmp(entries[i].components[offset], 
                        entries[j].components[offset]))
        {
            entries[j].n_children++;
            j++;
        }

        /* If we found a subtree then let's build it */
        if(j > i + 1)
            build_tree_postorder(i + 1,  j, entries[i].n_components - 1);
   
        /* Allocate memory for children */
        entries[j].children = malloc(entries[j].n_children * sizeof(entries[j].children[0]));
    
        /* Fill direct children */
        int n_children = 0;
        for(int k = i; k < j; k++)
            if(entries[k].n_components == offset + 2)
                entries[j].children[n_children++] = &entries[k];

        i = j;

        /* Ensure that the children have been allocated appropriately */
        //assert(n_children == entries[j].n_children);
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

void show_entries_raw(struct entry e[], int n) {
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

static void status(char *msg) {
    static int pass = 1;
    fprintf(stderr, "(%d) %s\n", pass++, msg);
} 

#ifdef DEBUG
/*
 *  Helper/testing function for displaying detailed information
 *  about entries that have been read in from du
 */ 
static void dispEntryDetail(struct entry e[], int n) {
    printf("Detailed Entries\n# of Entries: %d\n\n", n);

    for(int i = 0; i < n; i++) {
        printf("Index: %d\n", i);
        printf("Size: %" PRIu64 "\n", e[i].size);        
        printf("Depth: %d\n", e[i].depth);
        print("# Children: %d\n", e[i].n_children);
        print("# Components: %d\n", e[i].n_components);
        printf("Components: \n");

        if(e[i].n_components) {
            for(int j = 0; j < e[i].n_components; j++) {
                printf("%s/", e[i].components[j]);
            }
        }
       
        printf("\n");
    }
}

/*
 * Helper/testing function for displaying a simplified order
 * that entries are currently in - formatted for directory 
 * view. Includes information about the size.
 */
static void dispEntries(struct entry e[], int n) {
    printf("Simple Entries\n# of Entries: %d\n\n", n);

    for(int i = 0; i < n; i++) {
        if(e[i].components) {
            for(int j = 0; j < e[i].n_components; j++) {
                printf("%s/", e[i].components[j]);
                printf(" ," PRIu64 "\n", e[i].size);
            }
        }
    }
}

#endif

static char *iobuf;

int main(int argc, char **argv) {

    int c;
    int pflag = 0, gflag = 0, rflag = 0, zeroflag = 0;
    FILE *inf = stdin;

    while((c = getopt(argc, argv, "pgr0")) != -1)
    {
        switch(c) {
            case 'p':// Enable pre-order sorting
                pflag = 1;
                break;
            case 'g':// Enable GUI
                gflag = 1;
                break;
            case 'r':// Enable GUI
                rflag = 1;
                break;
            case '0':// Enable GUI
                zeroflag = 1;
                break;
            case '?':// Error handling
                fprintf(stderr, "Unknown option -%c\n", optopt);
                exit(1);
            default:// Something really weird happened
                abort();
        }
    }
    
    if (optind < argc) {
        if (optind < argc - 1) {
            fprintf(stderr, "extra argument(s)\n");
            exit(1);
        }
        fprintf(stderr, "open %s\n", argv[optind]);
        inf = fopen(argv[optind], "r");
        if (!inf) {
            perror("fopen");
            exit(1);
        }
    }

    // Set up for large IOs
    iobuf = malloc(IO_BUFFER_LENGTH);
    
    if (!iobuf) {
        perror("malloc(iobuf)");
        exit(1);
    }
    
    int result = setvbuf(inf, iobuf, _IOFBF, IO_BUFFER_LENGTH);
    
    if (result) {
        perror("setvbuf");
        exit(1);
    }

    // Read in data from du
    status("Parsing du file.");
    read_entries(inf, zeroflag);

    if (n_entries == 0)
        return 0;

    // default: post order
    if(pflag == 0)
    {
    }
    
    // pre order
    if(pflag) {
        status("Sorting entries.");
        qsort(entries, n_entries, sizeof(entries[0]), compare_entries);

        if(entries[0].n_components == 0) {
            fprintf(stderr, "Mysterious zero-length entry in table.\n");
            exit(1);
        }

        status("Building tree (preorder).");
        root_entry = &entries[0];
        base_depth = root_entry->n_components;
        build_tree_preorder(0, n_entries, 0);
    } else {
        status("Building tree (postorder).");
        root_entry = &entries[n_entries - 1];
        base_depth = root_entry->n_components;
        build_tree_postorder(0, n_entries, 0);
    }

    if (gflag) {
        status("Recording depths.");
        find_max_depths(root_entry);
        status("Rendering tree.");
        gui(argc, argv);
    } else if (rflag) {
        status("Emitting entries.");
        show_entries_raw(entries, n_entries);
    } else {
        status("Emitting tree.");
        show_entries(root_entry);
    }
    
    return(0); 
}
