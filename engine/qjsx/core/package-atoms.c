/*          _
 *  __ _   (_)  ___ __ __    qjsx - JavaScript for devices
 * / _` |  | | (_-< \ \ /    Author: Etienne Cochard + Opus 5.5
 * \__, | _/ | /__/ /_\_\    Copyright (c) 2026 R-LIBRE INGENIERIE
 *    |_| |__/               SPDX-License-Identifier: MIT (see LICENSE)
 */

/*
 * QuickJS - core/package-atoms.c
 *
 * qjsx: the names (atoms) of a package, made once by "qjsx build" and used
 * where the package lies (flash on a device). Not part of upstream QuickJS:
 * see doc/engine-changes.md (S).
 *
 * The atom indexes of a runtime that runs a package are fixed:
 *     0 .. QJSX_ROM_ATOM_COUNT - 1      the names of the engine (rom-atoms.c)
 *     then the names of the package     JS_SetPackageAtoms()
 *     then the names made at run time   in the heap, as in QuickJS
 * The bytecode of the package is written with these indexes
 * (JS_WRITE_OBJ_FIXED_ATOMS): nothing in it is changed when it is read, so
 * it runs where it lies (JS_READ_OBJ_ROM_DATA | JS_READ_OBJ_FIXED_ATOMS).
 *
 * The image of the names (little endian, at an address multiple of 4):
 *     JSPackageAtomsHeader
 *     uint32_t offsets[count]     of the JSString of each index from the
 *                                 start of the image; 0: no name there
 *     uint16_t hash[hash_size]    open addressing by JSString.hash: an atom
 *                                 index, 0 = empty slot
 *     padding to 8 bytes
 *     the names, each as QuickJS allocates an atom string: a block header
 *     with an immortal reference count, the JSString (its atom index in
 *     hash_next), the characters, padded to 8 bytes
 * It holds no pointer: the same image serves a 32 bit device and the PC.
 */

#include "quickjs-internal.h"

#if QJSX_ROM_ATOMS

#define JS_PACKAGE_ATOMS_MAGIC 0x4e534a51   /* "QJSN" */
/* free entries of the atom table after the names of the package (it then
   grows by half, as in __JS_NewAtom) */
#define JS_PACKAGE_RAM_ATOM_SLOTS 128
/* an atom index of the image fits in 16 bits */
#define JS_PACKAGE_ATOMS_MAX 60000

typedef struct JSPackageAtomsHeader {
    uint32_t magic;        /* JS_PACKAGE_ATOMS_MAGIC */
    uint32_t fingerprint;  /* of the names of the engine the indexes follow */
    uint32_t first_atom;   /* index of the first name: QJSX_ROM_ATOM_COUNT */
    uint32_t count;        /* indexes, the ones without name included */
    uint32_t hash_size;    /* slots of the hash table, a power of two */
    uint32_t size;         /* bytes of the image */
} JSPackageAtomsHeader;

uint32_t JS_GetAtomsFingerprint(void)
{
    return QJSX_ROM_ATOM_FINGERPRINT;
}

/* Gives the names of a package to a runtime that has made none of its own
   yet (just after JS_NewRuntime2, before any context). The image must stay
   valid and unchanged as long as the runtime. -1: not an image of names for
   this engine, or too late; nothing is changed. */
int JS_SetPackageAtoms(JSRuntime *rt, const uint8_t *image, size_t size)
{
    const JSPackageAtomsHeader *h = (const JSPackageAtomsHeader *)image;
    const uint32_t *offsets;
    JSAtomStruct **new_array;
    uint32_t i, first, new_size;
    size_t tables_end;

    /* an image for this engine */
    if (size < sizeof(*h) || ((uintptr_t)image & 3) != 0)
        return -1;
    if (h->magic != JS_PACKAGE_ATOMS_MAGIC || h->fingerprint != QJSX_ROM_ATOM_FINGERPRINT ||
        h->first_atom != QJSX_ROM_ATOM_COUNT || h->size > size || h->count > JS_PACKAGE_ATOMS_MAX ||
        h->hash_size == 0 || (h->hash_size & (h->hash_size - 1)) != 0)
        return -1;
    tables_end = sizeof(*h) + (size_t)h->count * 4 + (size_t)h->hash_size * 2;
    if (tables_end > h->size)
        return -1;

    /* a runtime that only has the names of the engine */
    first = QJSX_ROM_ATOM_COUNT;
    if (rt->const_atom_end != first || rt->atom_count != (int)first || rt->atom_free_index != (int)first)
        return -1;

    /* the atom table: the engine's names, the package's, free entries */
    offsets = (const uint32_t *)(h + 1);
    for(i = 0; i < h->count; i++) {
        if (offsets[i] != 0 && (offsets[i] < tables_end || offsets[i] + sizeof(JSString) > h->size))
            return -1;
    }
    new_size = first + h->count + JS_PACKAGE_RAM_ATOM_SLOTS;
    new_array = js_realloc_rt(rt, rt->atom_array, sizeof(rt->atom_array[0]) * new_size);
    if (!new_array)
        return -1;
    rt->atom_array = new_array;
    for(i = 0; i < h->count; i++) {
        if (offsets[i] != 0) {
            /* read-only: never written (immortal reference count) */
            new_array[first + i] = (JSAtomStruct *)(image + offsets[i]);
        } else {
            /* an index without name: never given to a new atom */
            new_array[first + i] = new_array[JS_ATOM_empty_string];
        }
    }
    for(i = first + h->count; i < new_size; i++) {
        uint32_t next = (i == new_size - 1) ? 0 : i + 1;
        new_array[i] = atom_set_free(next);
    }

    rt->atom_size = new_size;
    rt->atom_count += h->count;
    rt->atom_count_resize += h->count;
    rt->atom_free_index = first + h->count;
    rt->const_atom_end = first + h->count;
    rt->pkg_atom_hash = (const uint16_t *)(offsets + h->count);
    rt->pkg_atom_hash_size = h->hash_size;
    return 0;
}

JS_BOOL JS_HasPackageAtoms(JSRuntime *rt)
{
    return rt->pkg_atom_hash != NULL;
}

#ifndef QJSX_NO_COMPILER
/* The image of the names that 'ctx' made after the ones of the engine, for
   JS_SetPackageAtoms(): their indexes are kept. The runtime must have been
   used for nothing but reading the bytecode of the package (qjsx build).
   Allocated with js_malloc; NULL with an exception on error. */
uint8_t *JS_WritePackageAtoms(JSContext *ctx, size_t *psize)
{
    JSRuntime *rt = ctx->rt;
    JSPackageAtomsHeader *h;
    uint32_t *offsets;
    uint16_t *hash;
    uint8_t *image;
    uint32_t first = QJSX_ROM_ATOM_COUNT;
    uint32_t i, count, hash_size, slot;
    size_t pos, total, text_size;

    /* the indexes in use after the engine's: strings only */
    count = 0;
    total = 0;
    for(i = first; i < (uint32_t)rt->atom_size; i++) {
        JSAtomStruct *p = rt->atom_array[i];
        if (atom_is_free(p))
            continue;
        if (p->atom_type != JS_ATOM_TYPE_STRING) {
            JS_ThrowInternalError(ctx, "a name of the package is not a string");
            return NULL;
        }
        count = i - first + 1;
        text_size = ((size_t)p->len << p->is_wide_char) + 1 - p->is_wide_char;
        total += (sizeof(JSMallocBlockHeader) + sizeof(JSString) + text_size + 7) & ~(size_t)7;
    }
    if (count > JS_PACKAGE_ATOMS_MAX) {
        JS_ThrowInternalError(ctx, "too many names in the package");
        return NULL;
    }

    /* the tables, then the names */
    hash_size = 16;
    while (hash_size < 2 * count)
        hash_size *= 2;
    pos = (sizeof(*h) + (size_t)count * 4 + (size_t)hash_size * 2 + 7) & ~(size_t)7;
    total += pos;
    image = js_mallocz(ctx, total);
    if (!image)
        return NULL;
    h = (JSPackageAtomsHeader *)image;
    h->magic = JS_PACKAGE_ATOMS_MAGIC;
    h->fingerprint = QJSX_ROM_ATOM_FINGERPRINT;
    h->first_atom = first;
    h->count = count;
    h->hash_size = hash_size;
    h->size = total;
    offsets = (uint32_t *)(h + 1);
    hash = (uint16_t *)(offsets + count);

    for(i = first; i < first + count; i++) {
        JSAtomStruct *p = rt->atom_array[i];
        JSMallocBlockHeader *block;
        JSString *copy;
        if (atom_is_free(p))
            continue;

        /* the name, as an atom string with an immortal count */
        block = (JSMallocBlockHeader *)(image + pos);
        block->ref_count = JS_REF_COUNT_IMMORTAL;
        copy = (JSString *)(image + pos + sizeof(*block));
        text_size = ((size_t)p->len << p->is_wide_char) + 1 - p->is_wide_char;
        copy->len = p->len;
        copy->is_wide_char = p->is_wide_char;
        copy->hash = p->hash;
        copy->atom_type = JS_ATOM_TYPE_STRING;
        copy->hash_next = i;   /* its atom index */
        memcpy(copy->u.str8, p->u.str8, text_size);
        offsets[i - first] = pos + sizeof(*block);
        pos += (sizeof(*block) + sizeof(*copy) + text_size + 7) & ~(size_t)7;

        /* in the hash table */
        slot = p->hash & (hash_size - 1);
        while (hash[slot] != 0)
            slot = (slot + 1) & (hash_size - 1);
        hash[slot] = i;
    }
    *psize = total;
    return image;
}
#endif

#else /* !QJSX_ROM_ATOMS */

uint32_t JS_GetAtomsFingerprint(void)
{
    return 0;
}

int JS_SetPackageAtoms(JSRuntime *rt, const uint8_t *image, size_t size)
{
    return -1;
}

JS_BOOL JS_HasPackageAtoms(JSRuntime *rt)
{
    return FALSE;
}

#endif
