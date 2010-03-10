/*
 * ProFTPD - FTP server daemon
 * Copyright (c) 2010 The ProFTPD Project team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * As a special exemption, The ProFTPD Project and other respective copyright
 * holders give permission to link this program with OpenSSL, and distribute
 * the resulting executable, without including the source code for OpenSSL in
 * the source distribution.
 */

/* Symbol table hashes
 * $Id: stash.c,v 1.2 2010-03-06 18:28:21 castaglia Exp $
 */

#include "conf.h"

/* This local structure vastly speeds up symbol lookups. */
struct stash {
  struct stash *next,*prev;
  pool *sym_pool;
  const char *sym_name;
  pr_stash_type_t sym_type;
  module *sym_module;

  union {
    conftable *sym_conf;
    cmdtable *sym_cmd;
    authtable *sym_auth;
    cmdtable *sym_hook;
    void *sym_generic;
  } ptr;
};

/* Symbol hashes for each type */
static xaset_t *symbol_table[PR_TUNABLE_HASH_TABLE_SIZE];
static pool *symbol_pool = NULL;
static struct stash *curr_sym = NULL;

/* Symbol stash lookup code and management */

static struct stash *sym_alloc(void) {
  pool *sub_pool;
  struct stash *sym;

  /* XXX Use a smaller pool size, since there are lots of sub-pools allocated
   * for Stash symbols.  The default pool size (PR_TUNABLE_POOL_SIZE, 512
   * by default) is a bit large for symbols.
   */
  sub_pool = pr_pool_create_sz(symbol_pool, 128);

  sym = pcalloc(sub_pool, sizeof(struct stash));
  sym->sym_pool = sub_pool; 
  pr_pool_tag(sub_pool, "symbol");

  return sym;
}

static int sym_cmp(struct stash *s1, struct stash *s2) {
  int res;

  res = strcmp(s1->sym_name, s2->sym_name);

  /* Higher priority modules must go BEFORE lower priority in the
   * hash tables.
   */

  if (res == 0) {
    if (s1->sym_module != NULL &&
        s2->sym_module != NULL) {

      if (s1->sym_module->priority > s2->sym_module->priority) {
        return -1;
      }
    
      if (s1->sym_module->priority < s2->sym_module->priority) {
        return 1;
      }

      return res;
    }

    if (s1->sym_module != NULL &&
        s2->sym_module == NULL) {
      return -1;
    }

    if (s1->sym_module == NULL &&
        s2->sym_module != NULL) {
      return 1;
    }

    /* Both sym_module fields are null. */
    return 0;
  }

  return res;
}

static int symtab_hash(const char *name) {
  unsigned char *cp = NULL;
  int total = 0;

  if (!name)
    return 0;

  for (cp = (unsigned char *)name; *cp; cp++)
    total += (int)*cp;

  return (total < PR_TUNABLE_HASH_TABLE_SIZE ? total :
    (total % PR_TUNABLE_HASH_TABLE_SIZE));
}

int pr_stash_add_symbol(pr_stash_type_t sym_type, void *data) {
  struct stash *sym = NULL;
  int idx = 0;

  if (!data) {
    errno = EINVAL;
    return -1;
  }

  switch (sym_type) {
    case PR_SYM_CONF:
      sym = sym_alloc();
      sym->sym_type = PR_SYM_CONF;
      sym->sym_name = ((conftable *) data)->directive;
      sym->sym_module = ((conftable *) data)->m;
      sym->ptr.sym_conf = data;
      break;

    case PR_SYM_CMD:
      sym = sym_alloc();
      sym->sym_type = PR_SYM_CMD;
      sym->sym_name = ((cmdtable *) data)->command;
      sym->sym_module = ((cmdtable *) data)->m;
      sym->ptr.sym_cmd = data;
      break;

    case PR_SYM_AUTH:
      sym = sym_alloc();
      sym->sym_type = PR_SYM_AUTH;
      sym->sym_name = ((authtable *) data)->name;
      sym->sym_module = ((authtable *) data)->m;
      sym->ptr.sym_auth = data;
      break;

    case PR_SYM_HOOK:
      sym = sym_alloc();
      sym->sym_type = PR_SYM_HOOK;
      sym->sym_name = ((cmdtable *) data)->command;
      sym->sym_module = ((cmdtable *) data)->m;
      sym->ptr.sym_hook = data;
      break;

    default:
      errno = ENOENT;
      return -1;
  }

  /* XXX Should we check for null sym->sym_module as well? */
  if (sym->sym_name == NULL) {
    destroy_pool(sym->sym_pool);
    errno = EPERM;
    return -1;
  }

  /* XXX Ugly hack to support mixed cases of directives in config files. */
  if (sym_type != PR_SYM_CONF) {
    idx = symtab_hash(sym->sym_name);

  } else {
    register unsigned int i;
    char buf[1024];

    memset(buf, '\0', sizeof(buf));
    sstrncpy(buf, sym->sym_name, sizeof(buf)-1);

    for (i = 0; i < strlen(buf); i++)
      buf[i] = tolower((int) buf[i]);

    idx = symtab_hash(buf);
  }

  if (!symbol_table[idx]) {
    symbol_table[idx] = xaset_create(symbol_pool, (XASET_COMPARE) sym_cmp);
  }

  xaset_insert_sort(symbol_table[idx], (xasetmember_t *) sym, TRUE);
  return 0;
}

static struct stash *stash_lookup(pr_stash_type_t sym_type,
    const char *name, int idx) {
  struct stash *sym = NULL;

  if (symbol_table[idx]) {
    for (sym = (struct stash *) symbol_table[idx]->xas_list; sym;
        sym = sym->next) {
      if (sym->sym_type == sym_type &&
          (!name || strcasecmp(sym->sym_name, name) == 0)) {
        break;
      }
    }
  }

  return sym;
}

static struct stash *stash_lookup_next(pr_stash_type_t sym_type,
    const char *name, int idx, void *prev) {
  struct stash *sym = NULL;
  int last_hit = 0;

  if (symbol_table[idx]) {
    for (sym = (struct stash *) symbol_table[idx]->xas_list; sym;
        sym = sym->next) {
      if (last_hit && sym->sym_type == sym_type &&
          (!name || strcasecmp(sym->sym_name, name) == 0)) {
        break;
      }

      if (sym->ptr.sym_generic == prev) {
        last_hit++;
      }
    }
  }

  return sym;
}

void *pr_stash_get_symbol(pr_stash_type_t sym_type, const char *name,
    void *prev, int *idx_cache) {
  int idx;
  struct stash *sym = NULL;

  if (idx_cache &&
      *idx_cache != -1) {
    idx = *idx_cache;

  } else {

    /* XXX Ugly hack to support mixed cases of directives in config files. */
    if (sym_type != PR_SYM_CONF) {
      idx = symtab_hash(name);

    } else {
      register unsigned int i;
      char buf[1024];

      memset(buf, '\0', sizeof(buf));
      sstrncpy(buf, name, sizeof(buf)-1);

      for (i = 0; i < strlen(buf); i++)
        buf[i] = tolower((int) buf[i]);

      idx = symtab_hash(buf);
    }

    if (idx_cache)
      *idx_cache = idx;
  }

  if (idx >= PR_TUNABLE_HASH_TABLE_SIZE) {
    if (*idx_cache)
      *idx_cache = -1;

    errno = EINVAL;
    return NULL;
  }

  if (prev)
    curr_sym = sym = stash_lookup_next(sym_type, name, idx, prev);
  else
    curr_sym = sym = stash_lookup(sym_type, name, idx);

  switch (sym_type) {
    case PR_SYM_CONF:
      if (sym) {
        return sym->ptr.sym_conf;
      }

      errno = ENOENT;
      return NULL;

    case PR_SYM_CMD:
      if (sym) {
        return sym->ptr.sym_cmd;
      }

      errno = ENOENT;
      return NULL;

    case PR_SYM_AUTH:
      if (sym) {
        return sym->ptr.sym_auth;
      }

      errno = ENOENT;
      return NULL;

    case PR_SYM_HOOK:
      if (sym) {
        return sym->ptr.sym_hook;
      }

      errno = ENOENT;
      return NULL;
  }

  errno = EINVAL;
  return NULL;
}

int pr_stash_remove_symbol(pr_stash_type_t sym_type, const char *sym_name,
    module *sym_module) {
  int count = 0, symtab_idx = 0;

  if (!sym_name) {
    errno = EINVAL;
    return -1;
  }

  /* XXX Ugly hack to support mixed cases of directives in config files. */
  if (sym_type != PR_SYM_CONF) {
    symtab_idx = symtab_hash(sym_name);

  } else {
    register unsigned int i;
    char buf[1024];

    memset(buf, '\0', sizeof(buf));
    sstrncpy(buf, sym_name, sizeof(buf)-1);

    for (i = 0; i < strlen(buf); i++)
      buf[i] = tolower((int) buf[i]);

    symtab_idx = symtab_hash(buf);
  }

  switch (sym_type) {
    case PR_SYM_CONF: {
      int idx = -1;
      conftable *tab;

      tab = pr_stash_get_symbol(PR_SYM_CONF, sym_name, NULL, &idx);

      while (tab) {
        pr_signals_handle();

        /* Note: this works because of a hack: the symbol lookup functions
         * set a static pointer, curr_sym, to point to the struct stash
         * just looked up.  curr_sym will not be NULL if pr_stash_get_symbol()
         * returns non-NULL.
         */

        if (!sym_module ||
            curr_sym->sym_module == sym_module) {
          xaset_remove(symbol_table[symtab_idx], (xasetmember_t *) curr_sym);
          destroy_pool(curr_sym->sym_pool);
          curr_sym = NULL;
          tab = NULL;
          count++;
        }

        tab = pr_stash_get_symbol(PR_SYM_CONF, sym_name, tab, &idx);
      }

      break;
    }

    case PR_SYM_CMD: {
      int idx = -1;
      cmdtable *tab;

      tab = pr_stash_get_symbol(PR_SYM_CMD, sym_name, NULL, &idx);

      while (tab) {
        pr_signals_handle();

        /* Note: this works because of a hack: the symbol lookup functions
         * set a static pointer, curr_sym, to point to the struct stash
         * just looked up.  
         */

        if (!sym_module ||
            curr_sym->sym_module == sym_module) {
          xaset_remove(symbol_table[symtab_idx], (xasetmember_t *) curr_sym);
          destroy_pool(curr_sym->sym_pool);
          tab = NULL;
          count++;
        }

        tab = pr_stash_get_symbol(PR_SYM_CMD, sym_name, tab, &idx);
      }

      break;
    }

    case PR_SYM_AUTH: {
      int idx = -1;
      authtable *tab;

      tab = pr_stash_get_symbol(PR_SYM_AUTH, sym_name, NULL, &idx);

      while (tab) {
        pr_signals_handle();

        /* Note: this works because of a hack: the symbol lookup functions
         * set a static pointer, curr_sym, to point to the struct stash
         * just looked up.  
         */

        if (!sym_module ||
            curr_sym->sym_module == sym_module) {
          xaset_remove(symbol_table[symtab_idx], (xasetmember_t *) curr_sym);
          destroy_pool(curr_sym->sym_pool);
          tab = NULL;
          count++;
        }

        tab = pr_stash_get_symbol(PR_SYM_AUTH, sym_name, tab, &idx);
      }

      break;
    }

    case PR_SYM_HOOK: {
      int idx = -1;
      cmdtable *tab;

      tab = pr_stash_get_symbol(PR_SYM_HOOK, sym_name, NULL, &idx);

      while (tab) {
        pr_signals_handle();

        if (!sym_module ||
            curr_sym->sym_module == sym_module) {
          xaset_remove(symbol_table[symtab_idx], (xasetmember_t *) curr_sym);
          destroy_pool(curr_sym->sym_pool);
          tab = NULL;
          count++;
        }

        tab = pr_stash_get_symbol(PR_SYM_HOOK, sym_name, tab, &idx);
      }

      break;
    }

    default:
      errno = EINVAL;
      return -1;
  }

  return count;
}

int init_stash(void) {
  if (symbol_pool)
    destroy_pool(symbol_pool);

  symbol_pool = make_sub_pool(permanent_pool); 
  pr_pool_tag(symbol_pool, "Stash Pool");
  memset(symbol_table, '\0', sizeof(symbol_table));

  return 0;
}