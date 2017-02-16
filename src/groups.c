#include "groups.h"

#include "flist.h"
#include "debug.h"
#include "macros.h"
#include "main_native.h"
#include "self.h"
#include "settings.h"
#include "text.h"

#include "av/audio.h"
#include "ui/edit.h"
#include "ui/scrollable.h"

#include "layout/group.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <tox/tox.h>

GROUPCHAT *get_group(uint32_t group_number) {
    if (group_number >= UTOX_MAX_NUM_GROUPS) {
        LOG_ERR("get_group", " index: %u is out of bounds." , group_number);
        return NULL;
    }

    return &group[group_number];
}

void group_init(GROUPCHAT *g, uint32_t group_number, bool av_group) {
    pthread_mutex_lock(&messages_lock); /* make sure that messages has posted before we continue */
    if (!g->peer) {
        g->peer = calloc(MAX_GROUP_PEERS, sizeof(void));
    }

    g->name_length = snprintf((char *)g->name, sizeof(g->name), "Groupchat #%u", group_number);
    if (g->name_length >= sizeof(g->name)) {
        g->name_length = sizeof(g->name) - 1;
    }
    if (av_group) {
        g->topic_length = sizeof("Error creating voice group, not supported yet") - 1;
        strcpy2(g->topic, "Error creating voice group, not supported yet");
    } else {
        g->topic_length = sizeof("Drag friends to invite them") - 1;
        memcpy(g->topic, "Drag friends to invite them", sizeof("Drag friends to invite them") - 1);
    }

    g->msg.scroll               = 1.0;
    g->msg.panel.type           = PANEL_MESSAGES;
    g->msg.panel.content_scroll = &scrollbar_group;
    g->msg.panel.y              = MAIN_TOP;
    g->msg.panel.height         = CHAT_BOX_TOP;
    g->msg.panel.width          = -SCROLL_WIDTH;
    g->msg.is_groupchat         = 1;

    g->number   = group_number;
    g->notify   = settings.group_notifications;
    g->av_group = av_group;
    pthread_mutex_unlock(&messages_lock); /* make sure that messages has posted before we continue */

    flist_addgroup(g);
    flist_select_last();
}

uint32_t group_add_message(GROUPCHAT *g, uint32_t peer_id, const uint8_t *message, size_t length, uint8_t m_type) {
    pthread_mutex_lock(&messages_lock); /* make sure that messages has posted before we continue */
    const GROUP_PEER *peer = g->peer[peer_id];
    MESSAGES *m = &g->msg;

    MSG_HEADER *msg    = calloc(1, sizeof(MSG_HEADER));
    if (!msg) {
        LOG_ERR("Groupchats", " Unable to allocate memory for message header.");
        return UINT32_MAX;
    }
    msg->our_msg       = (g->our_peer_number == peer_id ? true : false);
    msg->msg_type      = m_type;

    msg->via.grp.length        = length;
    msg->via.grp.author_id     = peer_id;

    msg->via.grp.author_length = peer->name_length;
    msg->via.grp.author_color  = peer->name_color;
    time(&msg->time);

    msg->via.grp.author = calloc(1, peer->name_length);
    if (!msg->via.grp.author) {
        LOG_ERR("Groupchat", "Unable to allocate space for author nickname.");

        free(msg);
        return UINT32_MAX;
    }
    memcpy(msg->via.grp.author, peer->name, peer->name_length);

    msg->via.grp.msg = calloc(1, length);
    if (!msg->via.grp.msg) {
        LOG_ERR("Groupchat", "Unable to allocate space for message.");

        free(msg->via.grp.author);
        free(msg);
        return UINT32_MAX;
    }
    memcpy(msg->via.grp.msg, message, length);

    pthread_mutex_unlock(&messages_lock);

    return message_add_group(m, msg);
}

void group_peer_add(GROUPCHAT *g, uint32_t peer_id, bool UNUSED(our_peer_number), uint32_t name_color) {
    pthread_mutex_lock(&messages_lock); /* make sure that messages has posted before we continue */
    if (!g->peer) {
        g->peer = calloc(MAX_GROUP_PEERS, sizeof(void));
        LOG_NOTE("Groupchat", "Needed to calloc peers for this group chat. (%u)" , peer_id);
    }

    GROUP_PEER *peer = g->peer[peer_id];

    char *default_peer_name = "<unknown>";

    // Allocate space for the struct and the dynamic array holding the peer's name.
    peer = calloc(1, sizeof(GROUP_PEER) + strlen(default_peer_name) + 1);
    if (!peer) {
        LOG_ERR("Groupchat", "Unable to allocate space for group peer.");
        exit(42); // TODO: Header file for exit codes. This is just silly.
    }
    strcpy2(peer->name, default_peer_name);
    peer->name_length = 0;
    peer->name_color  = name_color;
    peer->id          = peer_id;

    g->peer[peer_id] = peer;
    g->peer_count++;
    pthread_mutex_unlock(&messages_lock);
}

void group_peer_del(GROUPCHAT *g, uint32_t peer_id) {
    group_add_message(g, peer_id, (const uint8_t *)"<- has Quit!", 12, MSG_TYPE_NOTICE);

    pthread_mutex_lock(&messages_lock); /* make sure that messages has posted before we continue */

    if (!g->peer) {
        LOG_TRACE("Groupchat", "Unable to del peer from NULL group" );
    }

    GROUP_PEER *peer = g->peer[peer_id];

    if (peer) {
        LOG_TRACE(__FILE__, "Freeing peer %u, name %.*s" , peer_id, (int)peer->name_length, peer->name);
        free(peer);
    } else {
        LOG_TRACE("Groupchat", "Unable to find peer for deletion" );
        return;
    }
    g->peer_count--;
    g->peer[peer_id] = NULL;
    pthread_mutex_unlock(&messages_lock);
}

void group_peer_name_change(GROUPCHAT *g, uint32_t peer_id, const uint8_t *name, size_t length) {
    pthread_mutex_lock(&messages_lock); /* make sure that messages has posted before we continue */
    if (!g->peer) {
        LOG_TRACE("Groupchat", "Unable to add peer to NULL group" );
        return;
    }

    GROUP_PEER *peer = g->peer[peer_id];

    if (peer && peer->name_length) {
        uint8_t old[TOX_MAX_NAME_LENGTH];
        uint8_t msg[TOX_MAX_NAME_LENGTH];
        size_t size = 0;

        memcpy(old, peer->name, peer->name_length);
        size = snprintf((char *)msg, TOX_MAX_NAME_LENGTH, "<- has changed their name from %.*s",
                        (int)peer->name_length, old);

        GROUP_PEER *new_peer = realloc(peer, sizeof(GROUP_PEER) + sizeof(char) * length);

        if (new_peer) {
            peer = new_peer;
        } else {
            free(peer);
            LOG_FATAL_ERR(40, "Groupchat", "Couldn't realloc for group peer name!");
            exit(40);
        }

        peer->name_length = utf8_validate(name, length);
        memcpy(peer->name, name, length);
        g->peer[peer_id] = peer;
        pthread_mutex_unlock(&messages_lock);
        group_add_message(g, peer_id, msg, size, MSG_TYPE_NOTICE);
        return;
    } else if (peer) {
        /* Hopefully, they just joined, because that's the UX message we're going with! */
        GROUP_PEER *new_peer = realloc(peer, sizeof(GROUP_PEER) + sizeof(char) * length);

        if (new_peer) {
            peer = new_peer;
        } else {
            LOG_FATAL_ERR(43, "Groupchat", "Unable to realloc for group peer who just joined.");
            exit(43);
        }

        peer->name_length = utf8_validate(name, length);
        memcpy(peer->name, name, length);
        g->peer[peer_id] = peer;
        pthread_mutex_unlock(&messages_lock);
        group_add_message(g, peer_id, (const uint8_t *)"<- has joined the chat!", 23, MSG_TYPE_NOTICE);
        return;
    } else {
        LOG_ERR("Groupchat", "We can't set a name for a null peer! %u" , peer_id);
        exit(41);
    }
}

void group_reset_peerlist(GROUPCHAT *g) {
    /* ARE YOU KIDDING... WHO THOUGHT THIS API WAS OKAY?! */
    for (size_t i = 0; i < g->peer_count; ++i) {
        if (g->peer[i]) {
            free(g->peer[i]);
        }
    }
    free(g->peer);
}

void group_free(GROUPCHAT *g) {
    for (size_t i = 0; i < g->edit_history_length; ++i) {
        free(g->edit_history[i]);
    }

    free(g->edit_history);

    group_reset_peerlist(g);

    for (size_t i = 0; i < g->msg.number; ++i) {
        free(g->msg.data[i]->via.grp.author);
        free(g->msg.data[i]->via.grp.msg);
        message_free(g->msg.data[i]);
    }
    free(g->msg.data);

    memset(g, 0, sizeof(GROUPCHAT));
}


void group_notify_msg(GROUPCHAT *g, const char *msg, size_t msg_length) {
    if (g->notify == GNOTIFY_NEVER) {
        return;
    }

    if (g->notify == GNOTIFY_HIGHLIGHTS && strstr(msg, self.name) == NULL) {
        return;
    }

    char title[g->name_length + 25];

    size_t title_length =
        snprintf(title, g->name_length + 25, "uTox new message in %.*s", (int)g->name_length, g->name);

    notify(title, title_length, msg, msg_length, g, 1);

    if (flist_get_selected()->data != g) {
        postmessage_audio(UTOXAUDIO_PLAY_NOTIFICATION, NOTIFY_TONE_FRIEND_NEW_MSG, 0, NULL);
    }
}
