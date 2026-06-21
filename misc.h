#ifndef _MISC_H
#define _MISC_H 1

// How the free() library routine should have been all along: null the pointer after freeing!
#define FREE(p) (free(p), p = NULL)
#endif

