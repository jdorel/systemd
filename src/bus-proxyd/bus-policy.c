/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include "xml.h"
#include "fileio.h"
#include "strv.h"
#include "conf-files.h"
#include "bus-internal.h"
#include "bus-policy.h"

static void policy_item_free(PolicyItem *i) {
        assert(i);

        free(i->interface);
        free(i->member);
        free(i->error);
        free(i->name);
        free(i->path);
        free(i);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(PolicyItem*, policy_item_free);

static int file_load(Policy *p, const char *path) {

        _cleanup_free_ char *c = NULL, *policy_user = NULL, *policy_group = NULL;
        _cleanup_(policy_item_freep) PolicyItem *i = NULL;
        void *xml_state = NULL;
        unsigned n_other = 0;
        const char *q;
        int r;

        enum {
                STATE_OUTSIDE,
                STATE_BUSCONFIG,
                STATE_POLICY,
                STATE_POLICY_CONTEXT,
                STATE_POLICY_USER,
                STATE_POLICY_GROUP,
                STATE_POLICY_OTHER_ATTRIBUTE,
                STATE_ALLOW_DENY,
                STATE_ALLOW_DENY_INTERFACE,
                STATE_ALLOW_DENY_MEMBER,
                STATE_ALLOW_DENY_ERROR,
                STATE_ALLOW_DENY_PATH,
                STATE_ALLOW_DENY_MESSAGE_TYPE,
                STATE_ALLOW_DENY_NAME,
                STATE_ALLOW_DENY_OTHER_ATTRIBUTE,
                STATE_OTHER,
        } state = STATE_OUTSIDE;

        enum {
                POLICY_CATEGORY_NONE,
                POLICY_CATEGORY_DEFAULT,
                POLICY_CATEGORY_MANDATORY,
                POLICY_CATEGORY_USER,
                POLICY_CATEGORY_GROUP
        } policy_category = POLICY_CATEGORY_NONE;

        unsigned line = 0;

        assert(p);

        r = read_full_file(path, &c, NULL);
        if (r < 0) {
                if (r == -ENOENT)
                        return 0;

                log_error("Failed to load %s: %s", path, strerror(-r));
                return r;
        }

        q = c;
        for (;;) {
                _cleanup_free_ char *name = NULL;
                int t;

                t = xml_tokenize(&q, &name, &xml_state, &line);
                if (t < 0) {
                        log_error("XML parse failure in %s: %s", path, strerror(-t));
                        return t;
                }

                switch (state) {

                case STATE_OUTSIDE:

                        if (t == XML_TAG_OPEN) {
                                if (streq(name, "busconfig"))
                                        state = STATE_BUSCONFIG;
                                else {
                                        log_error("Unexpected tag %s at %s:%u.", name, path, line);
                                        return -EINVAL;
                                }

                        } else if (t == XML_END)
                                return 0;
                        else if (t != XML_TEXT || !in_charset(name, WHITESPACE)) {
                                log_error("Unexpected token (1) at %s:%u.", path, line);
                                return -EINVAL;
                        }

                        break;

                case STATE_BUSCONFIG:

                        if (t == XML_TAG_OPEN) {
                                if (streq(name, "policy")) {
                                        state = STATE_POLICY;
                                        policy_category = POLICY_CATEGORY_NONE;
                                        free(policy_user);
                                        free(policy_group);
                                        policy_user = policy_group = NULL;
                                } else {
                                        state = STATE_OTHER;
                                        n_other = 0;
                                }
                        } else if (t == XML_TAG_CLOSE_EMPTY ||
                                   (t == XML_TAG_CLOSE && streq(name, "busconfig")))
                                state = STATE_OUTSIDE;
                        else if (t != XML_TEXT || !in_charset(name, WHITESPACE)) {
                                log_error("Unexpected token (2) at %s:%u.", path, line);
                                return -EINVAL;
                        }

                        break;

                case STATE_POLICY:

                        if (t == XML_ATTRIBUTE_NAME) {
                                if (streq(name, "context"))
                                        state = STATE_POLICY_CONTEXT;
                                else if (streq(name, "user"))
                                        state = STATE_POLICY_USER;
                                else if (streq(name, "group"))
                                        state = STATE_POLICY_GROUP;
                                else {
                                        log_warning("Attribute %s of <policy> tag unknown at %s:%u, ignoring.", name, path, line);
                                        state = STATE_POLICY_OTHER_ATTRIBUTE;
                                }
                        } else if (t == XML_TAG_CLOSE_EMPTY ||
                                   (t == XML_TAG_CLOSE && streq(name, "policy")))
                                state = STATE_BUSCONFIG;
                        else if (t == XML_TAG_OPEN) {
                                PolicyItemType it;

                                if (streq(name, "allow"))
                                        it = POLICY_ITEM_ALLOW;
                                else if (streq(name, "deny"))
                                        it = POLICY_ITEM_DENY;
                                else {
                                        log_warning("Unknown tag %s in <policy> %s:%u.", name, path, line);
                                        return -EINVAL;
                                }

                                assert(!i);
                                i = new0(PolicyItem, 1);
                                if (!i)
                                        return log_oom();

                                i->type = it;
                                state = STATE_ALLOW_DENY;

                        } else if (t != XML_TEXT || !in_charset(name, WHITESPACE)) {
                                log_error("Unexpected token (3) at %s:%u.", path, line);
                                return -EINVAL;
                        }

                        break;

                case STATE_POLICY_CONTEXT:

                        if (t == XML_ATTRIBUTE_VALUE) {
                                if (streq(name, "default")) {
                                        policy_category = POLICY_CATEGORY_DEFAULT;
                                        state = STATE_POLICY;
                                } else if (streq(name, "mandatory")) {
                                        policy_category = POLICY_CATEGORY_MANDATORY;
                                        state = STATE_POLICY;
                                } else {
                                        log_error("context= parameter %s unknown for <policy> at %s:%u.", name, path, line);
                                        return -EINVAL;
                                }
                        } else {
                                log_error("Unexpected token (4) at %s:%u.", path, line);
                                return -EINVAL;
                        }

                        break;

                case STATE_POLICY_USER:

                        if (t == XML_ATTRIBUTE_VALUE) {
                                free(policy_user);
                                policy_user = name;
                                name = NULL;
                                state = STATE_POLICY;
                        } else {
                                log_error("Unexpected token (5) in %s:%u.", path, line);
                                return -EINVAL;
                        }

                        break;

                case STATE_POLICY_GROUP:

                        if (t == XML_ATTRIBUTE_VALUE) {
                                free(policy_group);
                                policy_group = name;
                                name = NULL;
                                state = STATE_POLICY;
                        } else {
                                log_error("Unexpected token (6) at %s:%u.", path, line);
                                return -EINVAL;
                        }

                        break;

                case STATE_POLICY_OTHER_ATTRIBUTE:

                        if (t == XML_ATTRIBUTE_VALUE)
                                state = STATE_POLICY;
                        else {
                                log_error("Unexpected token (7) in %s:%u.", path, line);
                                return -EINVAL;
                        }

                        break;

                case STATE_ALLOW_DENY:

                        assert(i);

                        if (t == XML_ATTRIBUTE_NAME) {
                                PolicyItemClass ic;

                                if (startswith(name, "send_"))
                                        ic = POLICY_ITEM_SEND;
                                else if (startswith(name, "receive_"))
                                        ic = POLICY_ITEM_RECV;
                                else if (streq(name, "own"))
                                        ic = POLICY_ITEM_OWN;
                                else if (streq(name, "own_prefix"))
                                        ic = POLICY_ITEM_OWN_PREFIX;
                                else if (streq(name, "user"))
                                        ic = POLICY_ITEM_USER;
                                else if (streq(name, "group"))
                                        ic = POLICY_ITEM_GROUP;
                                else {
                                        log_error("Unknown attribute %s= at %s:%u, ignoring.", name, path, line);
                                        state = STATE_ALLOW_DENY_OTHER_ATTRIBUTE;
                                        break;
                                }

                                if (i->class != _POLICY_ITEM_CLASS_UNSET && ic != i->class) {
                                        log_error("send_ and receive_ fields mixed on same tag at %s:%u.", path, line);
                                        return -EINVAL;
                                }

                                i->class = ic;

                                if (ic == POLICY_ITEM_SEND || ic == POLICY_ITEM_RECV) {
                                        const char *u;

                                        u = strchr(name, '_');
                                        assert(u);

                                        u++;

                                        if (streq(u, "interface"))
                                                state = STATE_ALLOW_DENY_INTERFACE;
                                        else if (streq(u, "member"))
                                                state = STATE_ALLOW_DENY_MEMBER;
                                        else if (streq(u, "error"))
                                                state = STATE_ALLOW_DENY_ERROR;
                                        else if (streq(u, "path"))
                                                state = STATE_ALLOW_DENY_PATH;
                                        else if (streq(u, "type"))
                                                state = STATE_ALLOW_DENY_MESSAGE_TYPE;
                                        else if ((streq(u, "destination") && ic == POLICY_ITEM_SEND) ||
                                                 (streq(u, "sender") && ic == POLICY_ITEM_RECV))
                                                state = STATE_ALLOW_DENY_NAME;
                                        else {
                                                log_error("Unknown attribute %s= at %s:%u, ignoring.", name, path, line);
                                                state = STATE_ALLOW_DENY_OTHER_ATTRIBUTE;
                                                break;
                                        }
                                } else
                                        state = STATE_ALLOW_DENY_NAME;

                        } else if (t == XML_TAG_CLOSE_EMPTY ||
                                   (t == XML_TAG_CLOSE && streq(name, i->type == POLICY_ITEM_ALLOW ? "allow" : "deny"))) {

                                if (i->class == _POLICY_ITEM_CLASS_UNSET) {
                                        log_error("Policy not set at %s:%u.", path, line);
                                        return -EINVAL;
                                }

                                if (policy_category == POLICY_CATEGORY_DEFAULT)
                                        LIST_PREPEND(items, p->default_items, i);
                                else if (policy_category == POLICY_CATEGORY_MANDATORY)
                                        LIST_PREPEND(items, p->default_items, i);
                                else if (policy_category == POLICY_CATEGORY_USER) {
                                        PolicyItem *first;

                                        assert_cc(sizeof(uid_t) == sizeof(uint32_t));

                                        r = hashmap_ensure_allocated(&p->user_items, trivial_hash_func, trivial_compare_func);
                                        if (r < 0)
                                                return log_oom();

                                        first = hashmap_get(p->user_items, UINT32_TO_PTR(i->uid));
                                        LIST_PREPEND(items, first, i);

                                        r = hashmap_replace(p->user_items, UINT32_TO_PTR(i->uid), first);
                                        if (r < 0) {
                                                LIST_REMOVE(items, first, i);
                                                return log_oom();
                                        }
                                } else if (policy_category == POLICY_CATEGORY_GROUP) {
                                        PolicyItem *first;

                                        assert_cc(sizeof(gid_t) == sizeof(uint32_t));

                                        r = hashmap_ensure_allocated(&p->group_items, trivial_hash_func, trivial_compare_func);
                                        if (r < 0)
                                                return log_oom();

                                        first = hashmap_get(p->group_items, UINT32_TO_PTR(i->gid));
                                        LIST_PREPEND(items, first, i);

                                        r = hashmap_replace(p->group_items, UINT32_TO_PTR(i->gid), first);
                                        if (r < 0) {
                                                LIST_REMOVE(items, first, i);
                                                return log_oom();
                                        }
                                }

                                state = STATE_POLICY;
                                i = NULL;

                        } else if (t != XML_TEXT || !in_charset(name, WHITESPACE)) {
                                log_error("Unexpected token (8) at %s:%u.", path, line);
                                return -EINVAL;
                        }

                        break;

                case STATE_ALLOW_DENY_INTERFACE:

                        if (t == XML_ATTRIBUTE_VALUE) {
                                assert(i);
                                if (i->interface) {
                                        log_error("Duplicate interface at %s:%u.", path, line);
                                        return -EINVAL;
                                }

                                i->interface = name;
                                name = NULL;
                                state = STATE_ALLOW_DENY;
                        } else {
                                log_error("Unexpected token (9) at %s:%u.", path, line);
                                return -EINVAL;
                        }

                        break;

                case STATE_ALLOW_DENY_MEMBER:

                        if (t == XML_ATTRIBUTE_VALUE) {
                                assert(i);
                                if (i->member) {
                                        log_error("Duplicate member in %s:%u.", path, line);
                                        return -EINVAL;
                                }

                                i->member = name;
                                name = NULL;
                                state = STATE_ALLOW_DENY;
                        } else {
                                log_error("Unexpected token (10) in %s:%u.", path, line);
                                return -EINVAL;
                        }

                        break;

                case STATE_ALLOW_DENY_ERROR:

                        if (t == XML_ATTRIBUTE_VALUE) {
                                assert(i);
                                if (i->error) {
                                        log_error("Duplicate error in %s:%u.", path, line);
                                        return -EINVAL;
                                }

                                i->error = name;
                                name = NULL;
                                state = STATE_ALLOW_DENY;
                        } else {
                                log_error("Unexpected token (11) in %s:%u.", path, line);
                                return -EINVAL;
                        }

                        break;

                case STATE_ALLOW_DENY_PATH:

                        if (t == XML_ATTRIBUTE_VALUE) {
                                assert(i);
                                if (i->path) {
                                        log_error("Duplicate path in %s:%u.", path, line);
                                        return -EINVAL;
                                }

                                i->path = name;
                                name = NULL;
                                state = STATE_ALLOW_DENY;
                        } else {
                                log_error("Unexpected token (12) in %s:%u.", path, line);
                                return -EINVAL;
                        }

                        break;

                case STATE_ALLOW_DENY_MESSAGE_TYPE:

                        if (t == XML_ATTRIBUTE_VALUE) {
                                assert(i);

                                if (i->message_type != 0) {
                                        log_error("Duplicate message type in %s:%u.", path, line);
                                        return -EINVAL;
                                }

                                r = bus_message_type_from_string(name, &i->message_type);
                                if (r < 0) {
                                        log_error("Invalid message type in %s:%u.", path, line);
                                        return -EINVAL;
                                }

                                state = STATE_ALLOW_DENY;
                        } else {
                                log_error("Unexpected token (13) in %s:%u.", path, line);
                                return -EINVAL;
                        }

                        break;

                case STATE_ALLOW_DENY_NAME:

                        if (t == XML_ATTRIBUTE_VALUE) {
                                assert(i);
                                if (i->name) {
                                        log_error("Duplicate name in %s:%u.", path, line);
                                        return -EINVAL;
                                }

                                i->name = name;
                                name = NULL;
                                state = STATE_ALLOW_DENY;
                        } else {
                                log_error("Unexpected token (14) in %s:%u.", path, line);
                                return -EINVAL;
                        }

                        break;

                case STATE_ALLOW_DENY_OTHER_ATTRIBUTE:

                        if (t == XML_ATTRIBUTE_VALUE)
                                state = STATE_ALLOW_DENY;
                        else {
                                log_error("Unexpected token (15) in %s:%u.", path, line);
                                return -EINVAL;
                        }

                        break;

                case STATE_OTHER:

                        if (t == XML_TAG_OPEN)
                                n_other++;
                        else if (t == XML_TAG_CLOSE || t == XML_TAG_CLOSE_EMPTY) {

                                if (n_other == 0)
                                        state = STATE_BUSCONFIG;
                                else
                                        n_other--;
                        }

                        break;
                }
        }
}

int policy_load(Policy *p) {
        _cleanup_strv_free_ char **l = NULL;
        char **i;
        int r;

        assert(p);

        file_load(p, "/etc/dbus-1/system.conf");
        file_load(p, "/etc/dbus-1/system-local.conf");

        r = conf_files_list(&l, ".conf", NULL, "/etc/dbus-1/system.d/", NULL);
        if (r < 0) {
                log_error("Failed to get configuration file list: %s", strerror(-r));
                return r;
        }

        STRV_FOREACH(i, l)
                file_load(p, *i);

        return 0;
}

void policy_free(Policy *p) {
        PolicyItem *i, *first;

        if (!p)
                return;

        while ((i = p->default_items)) {
                LIST_REMOVE(items, p->default_items, i);
                policy_item_free(i);
        }

        while ((i = p->mandatory_items)) {
                LIST_REMOVE(items, p->mandatory_items, i);
                policy_item_free(i);
        }

        while ((first = hashmap_steal_first(p->user_items))) {

                while ((i = first)) {
                        LIST_REMOVE(items, first, i);
                        policy_item_free(i);
                }

                policy_item_free(i);
        }

        while ((first = hashmap_steal_first(p->group_items))) {

                while ((i = first)) {
                        LIST_REMOVE(items, first, i);
                        policy_item_free(i);
                }

                policy_item_free(i);
        }

        hashmap_free(p->user_items);
        hashmap_free(p->group_items);

        p->user_items = p->group_items = NULL;
}

static void dump_items(PolicyItem *i) {

        if (!i)
                return;

        printf("Type: %s\n"
               "Class: %s\n",
               policy_item_type_to_string(i->type),
               policy_item_class_to_string(i->class));

        if (i->interface)
                printf("Interface: %s\n",
                       i->interface);

        if (i->member)
                printf("Member: %s\n",
                       i->member);

        if (i->error)
                printf("Error: %s\n",
                       i->error);

        if (i->path)
                printf("Path: %s\n",
                       i->path);

        if (i->name)
                printf("Name: %s\n",
                       i->name);

        if (i->message_type != 0)
                printf("Message Type: %s\n",
                       bus_message_type_to_string(i->message_type));

        if (i->uid_valid) {
                _cleanup_free_ char *user;

                user = uid_to_name(i->uid);

                printf("User: %s\n",
                       strna(user));
        }

        if (i->gid_valid) {
                _cleanup_free_ char *group;

                group = gid_to_name(i->gid);

                printf("Group: %s\n",
                       strna(group));
        }

        if (i->items_next) {
                printf("--\n");
                dump_items(i->items_next);
        }
}

static void dump_hashmap_items(Hashmap *h) {
        PolicyItem *i;
        Iterator j;
        char *k;

        HASHMAP_FOREACH_KEY(i, k, h, j) {
                printf("Item for %s", k);
                dump_items(i);
        }
}

void policy_dump(Policy *p) {

        printf("→ Default Items:\n");
        dump_items(p->default_items);

        printf("→ Mandatory Items:\n");
        dump_items(p->mandatory_items);

        printf("→ Group Items:\n");
        dump_hashmap_items(p->group_items);

        printf("→ User Items:\n");
        dump_hashmap_items(p->user_items);
        exit(0);
}

static const char* const policy_item_type_table[_POLICY_ITEM_TYPE_MAX] = {
        [_POLICY_ITEM_TYPE_UNSET] = "unset",
        [POLICY_ITEM_ALLOW] = "allow",
        [POLICY_ITEM_DENY] = "deny",
};
DEFINE_STRING_TABLE_LOOKUP(policy_item_type, PolicyItemType);

static const char* const policy_item_class_table[_POLICY_ITEM_CLASS_MAX] = {
        [_POLICY_ITEM_CLASS_UNSET] = "unset",
        [POLICY_ITEM_SEND] = "send",
        [POLICY_ITEM_RECV] = "recv",
        [POLICY_ITEM_OWN] = "own",
        [POLICY_ITEM_OWN_PREFIX] = "own-prefix",
        [POLICY_ITEM_USER] = "user",
        [POLICY_ITEM_GROUP] = "group",
};
DEFINE_STRING_TABLE_LOOKUP(policy_item_class, PolicyItemClass);
