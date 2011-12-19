/*$6
 +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 */

#include "rumble.h"
#include "servers.h"
#include "comm.h"
#include "private.h"
#include "database.h"

/*
 =======================================================================================================================
    Main loop
 =======================================================================================================================
 */
void *rumble_imap_init(void *T) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumbleThread    *thread = (rumbleThread *) T;
    rumbleService   *svc = thread->svc;
    masterHandle    *master = svc->master;
    /* Initialize a session handle and wait for incoming connections. */
    sessionHandle   session;
    sessionHandle   *sessptr = &session;
    ssize_t         rc;
    char            *extra_data,
                    *cmd,
                    *parameters,
                    *line,
                    *tmp;
    const char      *myName;
    int             x = 0;
    sessionHandle   *s;
    accountSession  *pops;
    d_iterator      iter;
    svcCommandHook  *hook;
    c_iterator      citer;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    session.dict = dvector_init();
    session.recipients = dvector_init();
    session._svcHandle = (accountSession *) malloc(sizeof(accountSession));
    session._svc = svc;
    session.client = (clientHandle *) malloc(sizeof(clientHandle));
    session.client->tls = 0;
    session.client->send = 0;
    session.client->recv = 0;
    session._master = svc->master;
    pops = (accountSession *) session._svcHandle;
    pops->account = 0;
    pops->bag = 0;
    pops->folder = 0;
    session._tflags = RUMBLE_THREAD_IMAP;   /* Identify the thread/session as IMAP4 */
    myName = rrdict(master->_core.conf, "servername");
    myName = myName ? myName : "??";
    tmp = (char *) malloc(100);
    while (1) {
        comm_accept(svc->socket, session.client);
        pthread_mutex_lock(&svc->mutex);
        dvector_add(svc->handles, (void *) sessptr);
        svc->traffic.sessions++;
        pthread_mutex_unlock(&svc->mutex);
        session.flags = 0;
        session._tflags += 0x00100000;      /* job count ( 0 through 4095) */
        session.sender = 0;
        pops->bag = 0;
#if (RUMBLE_DEBUG & RUMBLE_DEBUG_COMM)
        rumble_debug("imap4", "Accepted connection from %s on IMAP4", session.client->addr);
#endif

        /* Check for hooks on accept() */
        rc = RUMBLE_RETURN_OKAY;
        rc = rumble_server_schedule_hooks(master, sessptr, RUMBLE_HOOK_ACCEPT + RUMBLE_HOOK_IMAP);
        if (rc == RUMBLE_RETURN_OKAY) rcprintf(sessptr, "* OK <%s> IMAP4rev1 Service Ready\r\n", myName);   /* Hello! */
        else svc->traffic.rejections++;

        /* Parse incoming commands */
        extra_data = (char *) malloc(32);
        cmd = (char *) malloc(32);
        parameters = (char *) malloc(1024);
        if (!cmd || !parameters || !extra_data) merror();
        while (rc != RUMBLE_RETURN_FAILURE) {
            memset(extra_data, 0, 32);
            memset(cmd, 0, 32);
            memset(parameters, 0, 1024);
            line = rumble_comm_read(sessptr);
            rc = 421;
            if (!line) break;
            rc = 105;   /* default return code is "500 unknown command thing" */
            if (sscanf(line, "%32s %32s %1000[^\r\n]", extra_data, cmd, parameters)) {
                rumble_string_upper(cmd);
                rumble_debug("imap4", "Client <%p> said: %s %s", &session, cmd, parameters);
                if (!strcmp(cmd, "UID")) {

                    /* Set UID flag if requested */
                    session.flags |= rumble_mailman_HAS_UID;
                    if (sscanf(parameters, "%32s %1000[^\r\n]", cmd, parameters)) rumble_string_upper(cmd);
                } else session.flags -= (session.flags & rumble_mailman_HAS_UID);   /* clear UID demand if not there. */
                cforeach((svcCommandHook *), hook, svc->commands, citer) {
                    if (!strcmp(cmd, hook->cmd)) rc = hook->func(master, &session, parameters, extra_data);
                }

                /*
                 * rumble_debug("imap4", "%s said: <%s> %s %s", session.client->addr, extra_data, cmd, parameters);
                 */
//                printf("Selected folder is: %"PRId64 "\r\n", pops->folder);
            }

            free(line);
            if (rc == RUMBLE_RETURN_IGNORE) {

                /*
                 * printf("Ignored command: %s %s\n",cmd, parameters);
                 */
                continue;   /* Skip to next line. */
            } else if (rc == RUMBLE_RETURN_FAILURE) {
                svc->traffic.rejections++;
                break;      /* Abort! */
            } else rcprintf(&session, "%s BAD Invalid command!\r\n", extra_data);       /* Bad command thing. */
        }

        /* Cleanup */
#if (RUMBLE_DEBUG & RUMBLE_DEBUG_COMM)
        rumble_debug("imap4", "Closing connection to %s on IMAP4", session.client->addr);
#endif
        if (rc == 421) {
            //rcprintf(&session, "%s BAD Session timed out!\r\n", extra_data); /* timeout! */
        }
        else {
            rcsend(&session, "* BYE bye!\r\n");
            rcprintf(&session, "%s OK <%s> signing off for now.\r\n", extra_data, myName);
        }

        /*$2
         ---------------------------------------------------------------------------------------------------------------
            Run hooks scheduled for service closing
         ---------------------------------------------------------------------------------------------------------------
         */

        //rumble_server_schedule_hooks(master, sessptr, RUMBLE_HOOK_CLOSE + RUMBLE_HOOK_IMAP);
        comm_addEntry(svc, session.client->brecv + session.client->bsent, session.client->rejected);
        disconnect(session.client->socket);

        /* Start cleanup */
        free(parameters);
        free(cmd);
        rumble_clean_session(sessptr);
        rumble_free_account(pops->account);
        rumble_mailman_close_bag(pops->bag);

        /* End cleanup */
        pthread_mutex_lock(&(svc->mutex));
        foreach((sessionHandle *), s, svc->handles, iter) {
            if (s == sessptr) {
                dvector_delete(&iter);
                x = 1;
                break;
            }
        }

        /* Check if we were told to go kill ourself :( */
        if ((session._tflags & RUMBLE_THREAD_DIE) || svc->enabled != 1 || thread->status == -1) {

            /*~~~~~~~~~~~~~~~*/
            rumbleThread    *t;
            /*~~~~~~~~~~~~~~~*/

#if RUMBLE_DEBUG & RUMBLE_DEBUG_THREADS
            printf("<imap4::threads>I (%#x) was told to die :(\n", (uintptr_t) pthread_self());
#endif
            cforeach((rumbleThread *), t, svc->threads, citer) {
                if (t == thread) {
                    cvector_delete(&citer);
                    break;
                }
            }

            pthread_mutex_unlock(&svc->mutex);
            pthread_exit(0);
        }

        pthread_mutex_unlock(&svc->mutex);
        myName = rrdict(master->_core.conf, "servername");
        myName = myName ? myName : "??";
    }

    pthread_exit(0);
}

/*
 =======================================================================================================================
 =======================================================================================================================
 */
ssize_t rumble_server_imap_login(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    char            user[256],
                    pass[256],
                    digest[1024];
    address         *addr;
    accountSession  *imap = (accountSession *) session->_svcHandle;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    rumble_mailman_close_bag(imap->bag);
    if (sscanf(parameters, "\"%256[^\" ]\" \"%256[^\" ]\"", user, pass) == 2 or sscanf(parameters, "%255s %255s", user, pass) == 2) {
        sprintf(digest, "<%s>", user);
        addr = rumble_parse_mail_address(digest);
        if (addr) {
            rumble_debug("imap4", "%s requested access to %s@%s via LOGIN\n", session->client->addr, addr->user, addr->domain);
            imap->account = rumble_account_data_auth(0, addr->user, addr->domain, pass);
            if (imap->account) {
                rumble_debug("imap4", "%s's request for %s@%s was granted\n", session->client->addr, addr->user, addr->domain);
                rcprintf(session, "%s OK Welcome!\r\n", extra_data);
                imap->folder = -1;
                imap->bag = rumble_mailman_open_bag(imap->account->uid);
            } else {
                rumble_debug("imap4", "%s's request for %s@%s was denied (wrong pass?)\n", session->client->addr, addr->user, addr->domain);
                rcprintf(session, "%s NO Incorrect username or password!\r\n", extra_data);
                session->client->rejected = 1;
            }
        } else {
            rcprintf(session, "%s NO Incorrect username or password!\r\n", extra_data);
            session->client->rejected = 1;
        }
    } else {
        rcprintf(session, "%s NO Incorrect username or password!\r\n", extra_data);
        session->client->rejected = 1;
    }

    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    NOOP
 =======================================================================================================================
 */
ssize_t rumble_server_imap_noop(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {
    rcprintf(session, "%s OK Doin' nothin'...\r\n", extra_data);
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    CAPABILITY
 =======================================================================================================================
 */
ssize_t rumble_server_imap_capability(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~*/
    char        capa[1024];
    char        *el;
    c_iterator  iter;
    /*~~~~~~~~~~~~~~~~~~~*/

    sprintf(capa, "* CAPABILITY");
    cforeach((char *), el, ((rumbleService *) session->_svc)->capabilities, iter) {
        sprintf(&capa[strlen(capa)], " %s", el);
    }

    sprintf(&capa[strlen(capa)], "\r\n");
    rcsend(session, capa);
    rcprintf(session, "%s OK CAPABILITY completed.\r\n", extra_data);
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    AUTHENTICATE
 =======================================================================================================================
 */
ssize_t rumble_server_imap_authenticate(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    accountSession  *imap = (accountSession *) session->_svcHandle;
    char            method[32],
                    tmp[258],
                    user[256],
                    pass[256],
                    *line,
                    *buffer;
    address         *addr = 0;
    int             x = 0;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    rumble_mailman_close_bag(imap->bag);
    if (sscanf(strchr(parameters, '"') ? strchr(parameters, '"') + 1 : parameters, "%32[a-zA-Z]", method)) {
        rumble_string_upper(method);
        if (!strcmp(method, "PLAIN")) {
            rcprintf(session, "%s OK Method <%s> accepted, input stuffs!\r\n", extra_data, method);
            line = rcread(session);
            if (line) {
                buffer = rumble_decode_base64(line);
                if (sscanf(buffer + 1, "\"%255[^\"]\"", user)) x = 2;
                else sscanf(buffer + 1, "%255s", user);
                if (!sscanf(buffer + 2 + x + strlen(user), "\"%255[^\"]\"", pass)) sscanf(buffer + 2 + x + strlen(user), "%255s", pass);
                sprintf(tmp, "<%s>", user);
                if (pass[strlen(pass) - 1] == 4) pass[strlen(pass) - 1] = 0;    /* remove EOT character if present. */
                addr = rumble_parse_mail_address(tmp);
                if (addr) {
                    rumble_debug("imap4", "%s requested access to %s@%s via AUTHENTICATE\n", session->client->addr, addr->user, addr->domain);
                    imap->account = rumble_account_data_auth(0, addr->user, addr->domain, pass);
                    if (imap->account) {
                        rcprintf(session, "%s OK Welcome!\r\n", extra_data);
                        imap->folder = -1;

                        /* Check if we have a shared mailbox instance available, if not, make one */
                        imap->bag = rumble_mailman_open_bag(imap->account->uid);
                    } else {
                        rcprintf(session, "%s NO Incorrect username or password!\r\n", extra_data);
                        session->client->rejected = 1;
                    }
                    rumble_free_address(addr);
                } else {
                    rcprintf(session, "%s NO Incorrect username or password!\r\n", extra_data);
                    session->client->rejected = 1;
                }

                free(buffer);
                free(line);
            }
        }
    }

    
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    SELECT
 =======================================================================================================================
 */
ssize_t rumble_server_imap_select(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    uint32_t                        exists,
                                    recent,
                                    first,
                                    found;
    rumble_args                     *params;
    rumble_mailman_shared_folder    *folder;
    char                            *selector;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    rumble_letter                   *letter;
    d_iterator                      iter;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    /* Are we authed? */
    if (imap->account) {
        params = rumble_read_words(parameters);
        found = 0;
        selector = params->argc ? params->argv[0] : "";

        /* Shared Object Reader Lock */
        rumble_rw_start_read(imap->bag->rrw);
        foreach(rmsf, folder, imap->bag->folders, iter) {
            if (!strcmp(selector, folder->name)) {
                imap->folder = folder->id;
                found++;
                break;
            }
        }

        if (!found && !strcmp(selector, "INBOX")) {
            imap->folder = 0;
            found++;
        }

        if (found) {
            session->flags |= rumble_mailman_HAS_SELECT;
            session->flags |= rumble_mailman_HAS_READWRITE;
            exists = 0;
            recent = 0;
            first = 0;
            folder = rumble_mailman_current_folder(imap);
            if (!folder) {
                rcprintf(session, "%s BAD Couldn't find the mailbox <%s>!\r\n", extra_data, selector);
                rumble_rw_stop_read(imap->bag->rrw);
                return (RUMBLE_RETURN_IGNORE);
            }

            /* Retrieve the statistics of the folder */
            foreach((rumble_letter *), letter, folder->letters, iter) {
                exists++;
                if (!first && (letter->flags & RUMBLE_LETTER_RECENT)) first = exists;
                if (letter->flags & RUMBLE_LETTER_RECENT) {
                    letter->flags -= RUMBLE_LETTER_RECENT;
                    recent++;
                }
            }

            rcprintf(session, "* %u EXISTS\r\n", exists);
            rcsend(session, "* FLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)\r\n");
            if (recent) rcprintf(session, "* %u RECENT.\r\n", recent);
            if (first) {
                rcprintf(session, "* OK [UNSEEN %"PRIu64 "] Message %"PRIu64 " is the first unseen message.\r\n", first, first);
            }

            rcprintf(session, "* OK [UIDVALIDITY %08u] UIDs valid\r\n", imap->account->uid);
            rcprintf(session, "%s OK [READ-WRITE] SELECT completed.\r\n", extra_data);
        } else {
            rcprintf(session, "%s NO No such mailbox <%s>!\r\n", extra_data, selector);
        }

        /* Shared Object Reader Unlock */
        rumble_rw_stop_read(imap->bag->rrw);
        rumble_args_free(params);
    } else {
        rcprintf(session, "%s NO Not logged in yet!\r\n", extra_data);
    }

    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    EXAMINE
 =======================================================================================================================
 */
ssize_t rumble_server_imap_examine(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    uint32_t                        exists,
                                    recent,
                                    first,
                                    found;
    rumble_args                     *params;
    rumble_mailman_shared_folder    *folder;
    char                            *selector;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    rumble_letter                   *letter;
    d_iterator                      iter;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    /* Are we authed? */
    if (imap->account) {
        params = rumble_read_words(parameters);
        found = 0;
        selector = params->argc ? params->argv[0] : "";

        /* Shared Object Reader Lock */
        rumble_rw_start_read(imap->bag->rrw);
        foreach(rmsf, folder, imap->bag->folders, iter) {
            if (!strcmp(selector, folder->name)) {
                imap->folder = folder->id;
                found++;
                break;
            }
        }

        if (!found && !strcmp(selector, "INBOX")) {
            imap->folder = 0;
            found++;
        }

        if (found) {
            session->flags |= rumble_mailman_HAS_SELECT;
            session->flags |= rumble_mailman_HAS_READWRITE;
            exists = 0;
            recent = 0;
            first = 0;
            folder = rumble_mailman_current_folder(imap);

            /* Retrieve the statistics of the folder */
            foreach((rumble_letter *), letter, folder->letters, iter) {
                exists++;
                if (!first && ((letter->flags & RUMBLE_LETTER_UNREAD) || (letter->flags == RUMBLE_LETTER_RECENT))) first = exists;
                if (letter->flags == RUMBLE_LETTER_RECENT) recent++;
            }

            rcprintf(session, "* %u EXISTS\r\n", exists);
            rcsend(session, "* FLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)\r\n");
            if (recent) rcprintf(session, "* %u RECENT.\r\n", recent);
            if (first) rcprintf(session, "* OK [UNSEEN %u] Message %u is the first unseen message.\r\n", first, first);
            rcprintf(session, "* OK [UIDVALIDITY %08u] UIDs valid\r\n", imap->account->uid);
            rcprintf(session, "%s OK [READ-ONLY] EXAMINE completed.\r\n", extra_data);
        } else {
            rcprintf(session, "%s NO No such mailbox <%s>!\r\n", extra_data, selector);
        }

        /* Shared Object Reader Unlock */
        rumble_rw_stop_read(imap->bag->rrw);
    } else {
        rcprintf(session, "%s NO Not logged in yet!\r\n", extra_data);
    }

    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    CREATE
 =======================================================================================================================
 */
ssize_t rumble_server_imap_create(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_args                     *args;
    char                            *newName;
    rumble_mailman_shared_folder    *folder,
                                    *newFolder;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    d_iterator                      iter;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    if (!imap->account) return RUMBLE_RETURN_IGNORE;
    
    args = rumble_read_words(parameters);
    newFolder = 0;
    if (args && args->argc == 1) {
        newName = args->argv[0];

        /* Shared Object Writer Lock */
        newFolder = rumble_mailman_get_folder(imap, newName);
        

        if (newFolder) {
             
            rcprintf(session, "%s NO CREATE failed: Duplicate folder name.\r\n", extra_data);
        }
        else {
            rumble_rw_start_write(imap->bag->rrw);
            /* Add the folder to the SQL DB */
            radb_run_inject(master->_core.db, "INSERT INTO folders (uid, name) VALUES (%u, %s)", imap->account->uid, newName);

            /* Update the local folder list */
            rumble_rw_stop_write(imap->bag->rrw);
            rumble_mailman_update_folders(imap->bag);
            rcprintf(session, "%s OK CREATE completed\r\n", extra_data);
        }

        /* Shared Object Writer Unlock */
       
    } else rcprintf(session, "%s BAD Invalid CREATE syntax!\r\n", extra_data);
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    DELETE
 =======================================================================================================================
 */
ssize_t rumble_server_imap_delete(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {
    rumble_letter                   *letter;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    d_iterator                      iter;
    rumble_args                     *args;
    rumble_mailman_shared_folder    *folder = 0;
    rumble_mailman_shared_folder    *pair;
    char                            *folderName = 0;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    /* Are we authed? */
    if (!imap->account) return RUMBLE_RETURN_IGNORE;
    args = rumble_read_words(parameters);
    
    // Find the folder we're looking for
    if (args && args->argc >= 1) {
        folderName = args->argv[0];
        folder = rumble_mailman_get_folder(imap, folderName);
    }
    if (!folder) {
        rcprintf(session, "%s NO DELETE failed: No such folder <%s>\r\n", extra_data, folderName);
        return (RUMBLE_RETURN_IGNORE);
    }
    

    /* Obtain write lock on the bag */
    rumble_rw_start_write(imap->bag->rrw);
    
    /* Mark all letters as deleted */
    
    foreach((rumble_letter *), letter, folder->letters, iter) {
        letter->flags |= RUMBLE_LETTER_EXPUNGE;
    }
    
    
    /* Commit the deletion */
    rumble_rw_stop_write(imap->bag->rrw);
    rumble_mailman_commit(imap, folder, 1);
    
    
    /* Delete folder from database and bag struct */
    rumble_rw_start_write(imap->bag->rrw);
    radb_run_inject(master->_core.db, "DELETE FROM folders WHERE id = %u", folder->id);
    foreach(rmsf, pair, imap->bag->folders, iter) {
        if (!strcmp(pair->name, folderName)) dvector_delete(&iter);
    }
    if (folder->name) free(folder->name);
    free(folder);
    rumble_rw_stop_write(imap->bag->rrw);
    
    rcprintf(session, "%s OK Deleted <%s>\r\n", extra_data, folderName);
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    RENAME
 =======================================================================================================================
 */
ssize_t rumble_server_imap_rename(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_args                     *args;
    char                            *oldName,
                                    *newName;
    rumble_mailman_shared_folder    *folder,
                                    *oldFolder,
                                    *newFolder;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    d_iterator                      iter;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    args = rumble_read_words(parameters);
    oldFolder = 0;
    newFolder = 0;
    if (args && args->argc == 2) {
        oldName = args->argv[0];
        newName = args->argv[1];

        /* Shared Object Writer Lock */
        oldFolder = rumble_mailman_get_folder(imap, oldName);
        newFolder = rumble_mailman_get_folder(imap, newName);
        rumble_rw_start_write(imap->bag->rrw);
        
        if (newFolder) rcprintf(session, "%s NO RENAME failed: Duplicate folder name.\r\n", extra_data);
        else if (!oldFolder)
            rcprintf(session, "%s NO RENAME failed: No such folder <%s>\r\n", extra_data, oldName);
        else {
            radb_run_inject(master->_core.db, "UPDATE folders set name = %s WHERE id = %u", newName, oldFolder->id);
            free(oldFolder->name);
            oldFolder->name = (char *) calloc(1, strlen(newName) + 1);
            strncpy(oldFolder->name, newName, strlen(newName));
            rcprintf(session, "%s OK RENAME completed\r\n", extra_data);
            oldFolder->updated = time(0);
        }

        /* Shared Object Writer Unlock */
        rumble_rw_stop_write(imap->bag->rrw);
    } else rcprintf(session, "%s BAD Invalid RENAME syntax!\r\n", extra_data);
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    SUBSCRIBE
 =======================================================================================================================
 */
ssize_t rumble_server_imap_subscribe(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_args                     *args;
    char                            *folderName;
    rumble_mailman_shared_folder    *pair,
                                    *folder;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    d_iterator                      iter;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    args = rumble_read_words(parameters);
    folder = 0;
    if (args && args->argc == 1) {
        folderName = args->argv[0];

        /* Shared Object Writer Lock */
        folder = rumble_mailman_get_folder(imap, folderName);
        if (!folder) rcprintf(session, "%s NO SUBSCRIBE failed: No such folder <%s>\r\n", extra_data, folderName);
        else {
            rumble_rw_start_write(imap->bag->rrw);
            radb_run_inject(master->_core.db, "UPDATE folders set subscribed = true WHERE id = %u", folder->id);
            folder->subscribed = 1;
            rumble_rw_stop_write(imap->bag->rrw);
            rcprintf(session, "%s OK SUBSCRIBE completed\r\n", extra_data);
        }

        /* Shared Object Writer Unlock */
        
    } else rcprintf(session, "%s BAD Invalid SUBSCRIBE syntax!\r\n", extra_data);
    rumble_args_free(args);
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    UNSUBSCRIBE
 =======================================================================================================================
 */
ssize_t rumble_server_imap_unsubscribe(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_args                     *args;
    char                            *folderName;
    rumble_mailman_shared_folder    *pair,
                                    *folder;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    d_iterator                      iter;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    args = rumble_read_words(parameters);
    folder = 0;
    if (args && args->argc == 1) {
        folderName = args->argv[0];

        /* Shared Object Writer Lock */
        folder = rumble_mailman_get_folder(imap, folderName);

        if (!folder) rcprintf(session, "%s NO UNSUBSCRIBE failed: No such folder <%s>\r\n", extra_data, folderName);
        else {
            rumble_rw_start_write(imap->bag->rrw);
            radb_run_inject(master->_core.db, "UPDATE folders set subscribed = false WHERE id = %u", folder->id);
            folder->subscribed = 0;
            rumble_rw_stop_write(imap->bag->rrw);
            rcprintf(session, "%s OK UNSUBSCRIBE completed\r\n", extra_data);            
        }

        /* Shared Object Writer Unlock */
        
    } else rcprintf(session, "%s BAD Invalid UNSUBSCRIBE syntax!\r\n", extra_data);
    rumble_args_free(args);
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    LIST
 =======================================================================================================================
 */
ssize_t rumble_server_imap_list(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_args                     *args;
    char                            *mbox,
                                    *pattern;
    rumble_mailman_shared_folder    *pair;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    d_iterator                      iter;
    rumble_mailman_shared_folder    *folder;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    args = rumble_read_words(parameters);
    if (args && args->argc == 2) {
        mbox = args->argv[0];
        pattern = args->argv[1];

        /* Shared Object Reader Lock */
        rumble_rw_start_read(imap->bag->rrw);
        folder = rumble_mailman_current_folder(imap);
        if (!folder) rcsend(session, "* LIST (\\Noselect) \".\" \"\"\r\n");
        else rcprintf(session, "* LIST (\\Noselect) \"\" \"%s\"\r\n", folder->name);
        foreach(rmsf, pair, imap->bag->folders, iter) {
            if (!strlen(pattern) || !strncmp(pair->name, pattern, strlen(pattern))) {
                rcprintf(session, "* LIST () \".\" \"%s\"\r\n", pair->name);
            }
        }

        /* Shared Object Reader Unlock */
        rumble_rw_stop_read(imap->bag->rrw);
        rcprintf(session, "%s OK LIST completed\r\n", extra_data);
    } else rcprintf(session, "%s BAD Invalid LIST syntax!\r\n", extra_data);
    rumble_args_free(args);
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    LSUB
 =======================================================================================================================
 */
ssize_t rumble_server_imap_lsub(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_args                     *args;
    char                            *mbox,
                                    *pattern,
                                    *pfolder;
    rumble_mailman_shared_folder    *pair;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    d_iterator                      iter;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    args = rumble_read_words(parameters);
    if (args && args->argc == 2) {
        mbox = args->argv[0];
        pattern = args->argv[1];

        /* Shared Object Reader Lock */
        rumble_rw_start_read(imap->bag->rrw);
        if (imap->folder != -1) {
            foreach(rmsf, pair, imap->bag->folders, iter) {
                if (pair->id == imap->folder) pfolder = pair->name;
                break;
            }
        }

        foreach(rmsf, pair, imap->bag->folders, iter) {
            if (pair->subscribed) rcprintf(session, "* LSUB () \".\" \"%s\"\r\n", pair->name);
        }

        /* Shared Object Reader Unlock */
        rumble_rw_stop_read(imap->bag->rrw);
        rcprintf(session, "%s OK LSUB completed\r\n", extra_data);
        printf("listed subscribed stuff\n");
    } else rcprintf(session, "%s BAD Invalid LSUB syntax!\r\n", extra_data);
    rumble_args_free(args);
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    STATUS
 =======================================================================================================================
 */
ssize_t rumble_server_imap_status(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    int                             messages = 0,
                                    recent = 0,
                                    unseen = 0,
                                    x = 0;
    rumble_letter                   *letter;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    d_iterator                      iter;
    rumble_args                     *args;
    rumble_mailman_shared_folder    *folder = 0;
    rumble_mailman_shared_folder    *pair;
    char                            *folderName = 0;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    args = rumble_read_words(parameters);
    
    // Find the folder we're looking for
    folder = 0;
    if (args && args->argc >= 1) {
        folderName = args->argv[0];
        folder = rumble_mailman_get_folder(imap, folderName);
        if (!folder) {
            rcprintf(session, "%s NO STATUS failed: No such folder <%s>\r\n", extra_data, folderName);
            return (RUMBLE_RETURN_IGNORE);
        }
    }

    /* Retrieve the status of the folder */
    rumble_rw_start_read(imap->bag->rrw);
    foreach((rumble_letter *), letter, folder->letters, iter) {
        if ((letter->flags & RUMBLE_LETTER_UNREAD) || (letter->flags == RUMBLE_LETTER_RECENT)) unseen++;
        if (letter->flags == RUMBLE_LETTER_RECENT) recent++;
        messages++;
    }

    rumble_rw_stop_read(imap->bag->rrw);
    rcprintf(session, "%s STATUS %s ", extra_data, folderName);
    for (x = 1; x < args->argc; x++) {
        if (strstr(args->argv[x], "UNSEEN")) rcprintf(session, "UNSEEN %u ", unseen);
        if (strstr(args->argv[x], "RECENT")) rcprintf(session, "RECENT %u ", recent);
        if (strstr(args->argv[x], "MESSAGES")) rcprintf(session, "MESSAGES %u ", messages);
    }

    rcprintf(session, "\r\n");
    rumble_args_free(args);
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    APPEND
 =======================================================================================================================
 */
ssize_t rumble_server_imap_append(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_args                     *params;
    char                            *destFolder;
    char                            *Flags;
    uint32_t                        size = 0;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    d_iterator                      iter;
    rumble_mailman_shared_folder    *folder,
                                    dFolder;
    int                             foundFolder = 0,
                                    readBytes = 0,
                                    flags = 0;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    dFolder.id = 0;
    params = rumble_read_words(parameters);
    if (params->argc > 1 && imap->bag) {
        printf("getting size of email\n");
        sscanf(params->argv[params->argc - 1], "{%d", &size);
        printf("size is %u bytes\n", size);
        destFolder = params->argv[0];
        Flags = params->argc > 2 ? params->argv[1] : "";

        /* Shared Object Reader Lock */
        rumble_rw_start_read(imap->bag->rrw);
        foreach(rmsf, folder, imap->bag->folders, iter) {
            if (!strcmp(destFolder, folder->name)) {
                dFolder.id = folder->id;
                foundFolder++;
                break;
            }
        }

        rumble_rw_stop_read(imap->bag->rrw);
    }

    /*
     * rcprintf(session, "FLAGS (%s%s%s%s) ", (letter->flags == RUMBLE_LETTER_RECENT) ? "\\Recent " : "", ;
     * / (letter->flags & RUMBLE_LETTER_READ) ? "\\Seen " : "", (letter->flags & RUMBLE_LETTER_DELETED) ?
     * "\\Deleted " : "", ;
     * (letter->flags & RUMBLE_LETTER_FLAGGED) ? "\\Flagged " : "");
     */
    if (strlen(Flags)) {
        if (strstr(Flags, "\\Seen")) flags |= RUMBLE_LETTER_READ;
        if (strstr(Flags, "\\Recent")) flags |= RUMBLE_LETTER_RECENT;
        if (strstr(Flags, "\\Deleted")) flags |= RUMBLE_LETTER_DELETED;
        if (strstr(Flags, "\\Flagged")) flags |= RUMBLE_LETTER_FLAGGED;
    }

    rumble_args_free(params);
    if (!size or!foundFolder) {
        rcprintf(session, "%s BAD Invalid APPEND syntax!\r\n", extra_data);
    } else {

        /*~~~~~~~~~~~~~~*/
        char    *sf;
        char    *fid;
        char    *filename;
        FILE    *fp = 0;
        /*~~~~~~~~~~~~~~*/

        rumble_debug("imap4", "Append required, making up new filename");
        fid = rumble_create_filename();
        sf = (char *) rumble_config_str(master, "storagefolder");
        filename = (char *) calloc(1, strlen(sf) + 36);
        if (!filename) merror();
        sprintf(filename, "%s/%s.msg", sf, fid);
        rumble_debug("imap4", "Storing new message of size %u in folder", size);
        fp = fopen(filename, "wb");
        if (fp) {

            /*~~~~~~~~~~~~~~~~*/
            char    *line;
            char    OK = 1;
            /*~~~~~~~~~~~~~~~~*/

            rumble_debug("imap4", "Writing to file %s", filename);
            rcprintf(session, "%s OK Appending!\r\n", extra_data);  /* thunderbird bug?? yes it is! */
            while (readBytes < size) {
                line = rumble_comm_read_bytes(session, size > 1024 ? 1024 : size);
                if (line) {
                    readBytes += strlen(line);
                    fwrite(line, strlen(line), 1, fp);
                    free(line);
                } else {
                    OK = 0;
                    break;
                }
            }

            fclose(fp);
            if (!OK) {
                rumble_debug("imap4", "An error occured while reading file from client");
                unlink(filename);
            } else {
                rumble_debug("imap4", "File written OK");
                radb_run_inject(master->_core.mail, "INSERT INTO mbox (id,uid, fid, size, flags, folder) VALUES (NULL,%u, %s, %u,%u, %l)",
                                imap->account->uid, fid, size, flags, dFolder.id);
                rumble_debug("imap4", "Added message no. #%s to folder %llu of user %u", fid, dFolder.id, imap->account->uid);
            }
        }

        free(filename);
        free(fid);

        /* TODO: Check if there's room for storing message */
    }

    /* 003 APPEND saved-messages (\Seen) {310} */
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    CHECK
 =======================================================================================================================
 */
ssize_t rumble_server_imap_check(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    CLOSE
 =======================================================================================================================
 */
ssize_t rumble_server_imap_close(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_mailman_shared_folder    *folder;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    
    folder = rumble_mailman_current_folder(imap);
    if (folder && imap->account && (session->flags & rumble_mailman_HAS_SELECT)) {
        rumble_mailman_commit(imap, folder, 0);
        session->flags -= rumble_mailman_HAS_SELECT;    /* clear select flag. */
        imap->folder = -1;
        rcprintf(session, "%s OK Expunged and closed the mailbox.\r\n", extra_data);
    } else rcprintf(session, "%s NO CLOSE: No mailbox to close!\r\n", extra_data);
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    EXPUNGE
 =======================================================================================================================
 */
ssize_t rumble_server_imap_expunge(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    rumble_mailman_shared_folder    *folder = rumble_mailman_current_folder(imap);
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    if (imap->account && (session->flags & rumble_mailman_HAS_SELECT)) {
        rumble_mailman_commit(imap, folder, 0);
        rcprintf(session, "%s OK Expunged them letters.\r\n", extra_data);
    } else rcprintf(session, "%s NO EXPUNGE: No mailbox selected for expunging!\r\n", extra_data);
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    SEARCH
 =======================================================================================================================
 */
ssize_t rumble_server_imap_search(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    FETCH
 =======================================================================================================================
 */
ssize_t rumble_server_imap_fetch(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    rumble_letter                   *letter;
    rumble_args                     *parts,
                                    *params;
    rumble_mailman_shared_folder    *folder;
    size_t                          a,
                                    b,
                                    c,
                                    d,
                                    w_uid,
                                    first,
                                    last;
    char                            line[1024],
                                    range[1024];
    const char                      *body,
                                    *body_peek;
    int                             flags,
                                    uid,
                                    internaldate,
                                    envelope;
    int                             size,
                                    text,
                                    x,
                                    header; /* rfc822.size/text/header */
    uint32_t                        octets;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    d_iterator                      iter;
    rangePair                       ranges[64];
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    folder = rumble_mailman_current_folder(imap);
    if (!folder) {
        rcprintf(session, "%s NO No mailbox selected for fetching!\r\n", extra_data);
        return (RUMBLE_RETURN_IGNORE);
    }

    rumble_mailman_scan_incoming(folder);
    uid = strstr(parameters, "UID") ? 1 : 0;
    internaldate = strstr(parameters, "INTERNALDATE") ? 1 : 0;
    envelope = strstr(parameters, "ENVELOPE") ? 1 : 0;
    size = strstr(parameters, "RFC822.SIZE") ? 1 : 0;
    text = strstr(parameters, "RFC822.TEXT") ? 1 : 0;
    header = strstr(parameters, "RFC822.HEADER") ? 1 : 0;
    flags = strstr(parameters, "FLAGS") ? 1 : 0;
    octets = 0;
    memset(line, 0, 1024);
    body_peek = strstr(parameters, "BODY.PEEK[");
    body = strstr(parameters, "BODY[");
    parts = 0;
    if (body) sscanf(body, "BODY[%1000[^]]<%u>", line, &octets);
    else if (body_peek)
        sscanf(body_peek, "BODY.PEEK[%1000[^]]<%u>", line, &octets);
    w_uid = session->flags & rumble_mailman_HAS_UID;
    if (body || body_peek) {
        if (strlen(line)) {

            /*~~~~~~~~~~~~~~~~~*/
            char    region[32],
                    buffer[1024];
            /*~~~~~~~~~~~~~~~~~*/

            memset(region, 0, 32);
            memset(buffer, 0, 1024);
            if (sscanf(line, "%32s (%1000c)", region, buffer) == 2) {
                parts = rumble_read_words(buffer);
                for (b = 0; b < parts->argc; b++) rumble_string_lower(parts->argv[b]);
            }
        }
    }

    params = rumble_read_words(parameters);
    rumble_scan_ranges(&ranges, params->argc > 0 ? params->argv[0] : "0");
    for (x = 0; ranges[x].start != 0; x++) {
        first = ranges[x].start;
        last = ranges[x].end;
        b = 0;
        a = 0;
        d = 0;
        printf("Fetching letter %lu through %lu\n", first, last);
        foreach((rumble_letter *), letter, folder->letters, iter) {
            a++;
            if (w_uid && (letter->id < first || (last > 0 && letter->id > last))) continue;
            if (!w_uid && (a < first || (last > 0 && a > last))) continue;
            d++;
            rcprintf(session, "* %u FETCH (", a);
            if (flags) {
                rcprintf(session, "FLAGS (%s%s%s%s) ", (letter->flags == RUMBLE_LETTER_RECENT) ? "\\Recent " : "",
                         (letter->flags & RUMBLE_LETTER_READ) ? "\\Seen " : "", (letter->flags & RUMBLE_LETTER_DELETED) ? "\\Deleted " : "",
                         (letter->flags & RUMBLE_LETTER_FLAGGED) ? "\\Flagged " : "");
            }

            if (uid || w_uid) rcprintf(session, "UID %llu ", letter->id);
            if (size) rcprintf(session, "RFC822.SIZE %u ", letter->size);
            if (internaldate) rcprintf(session, "INTERNALDATE %u ", letter->delivered);
            if (body) letter->flags -= (letter->flags & RUMBLE_LETTER_RECENT);  /* Remove \Recent flag since we're not peeking. */
            if (body || body_peek) {

                /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
                char    header[10240],
                        key[64];
                FILE    *fp = rumble_letters_open(imap->account, letter);
                /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

                if (fp) {
                    if (parts) {
                        memset(header, 0, 10240);
                        while (fgets(line, 1024, fp)) {
                            c = strlen(line);
                            if (line[0] == '\r' || line[0] == '\n') break;
                            memset(key, 0, 64);
                            if (sscanf(line, "%63[^:]", key)) {
                                rumble_string_lower(key);
                                if (parts) {
                                    for (b = 0; b < parts->argc; b++) {
                                        if (!strcmp(key, parts->argv[b])) {
                                            if (line[c - 2] != '\r') {
                                                line[c - 1] = '\r';
                                                line[c] = '\n';
                                                line[c + 1] = 0;
                                            }

                                            strncpy(header + strlen(header), line, strlen(line));
                                        }
                                    }
                                } else {

                                    /*
                                     * if ( line[c-2] != '\r' ) {line[c-1] = '\r';
                                     * line[c] = '\n';
                                     * line[c+1] = 0;
                                     */
                                    strncpy(header + strlen(header), line, strlen(line));
                                }
                            }
                        }

                        sprintf(header + strlen(header), "\r\n \r\n");
                        rcprintf(session, "BODY[HEADER.FIELDS (%s)] {%u}\r\n", line, strlen(header));
                        rcsend(session, header);

                        /*
                         * printf("BODY[HEADER.FIELDS (%s)] {%u}\r\n", line, strlen(header));
                         * printf("%s", header);
                         */
                    } else {
                        rcprintf(session, "BODY[] {%u}\r\n", letter->size);

                        /*
                         * printf("BODY[] {%u}\r\n", letter->size);
                         */
                        memset(line, 0, 1024);
                        while (fgets(line, 1024, fp)) {
                            rcsend(session, line);

                            /*
                             * printf("%s", line);
                             */
                        }
                    }

                    fclose(fp);
                } else printf("meh, couldn't open letter file\n");
                rcsend(session, " ");
            }

            rcprintf(session, ")\r\n");
        }
    }
    if (parts) rumble_args_free(parts);
    rumble_args_free(params);
    rcprintf(session, "%s OK FETCH completed\r\n", extra_data);
    if (folder) printf("Fetched %lu letters from <%s>\n", d, folder->name);
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    STORE
 =======================================================================================================================
 */
ssize_t rumble_server_imap_store(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    int                             first,
                                    last,
                                    silent,
                                    control,
                                    a,
                                    useUID,
                                    flag;
    rumble_letter                   *letter;
    char                            args[100],
                                    smurf[4];
    /* Check for selected folder */
    rumble_mailman_shared_folder    *folder;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    d_iterator                      iter;
    rumble_args                     *parts;
    rangePair                       ranges[64];
    int                             x = 0;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    folder = rumble_mailman_current_folder(imap);
    if (!folder) {
        rcprintf(session, "%s NO STORE: No mailbox selected for storing!\r\n", extra_data);
        return (RUMBLE_RETURN_IGNORE);
    }

    useUID = session->flags & rumble_mailman_HAS_UID;

    /* Get the store type */
    silent = strstr(parameters, ".SILENT") ? 1 : 0;
    control = strchr(parameters, '+') ? 1 : (strchr(parameters, '-') ? -1 : 0);
    memset(args, 0, 100);
    sscanf(parameters, "%*100[^(](%99[^)])", args);

    /* Set the master flag */
    flag = 0;
    flag |= strstr(parameters, "\\Deleted") ? RUMBLE_LETTER_DELETED : 0;
    flag |= strstr(parameters, "\\Seen") ? RUMBLE_LETTER_READ : 0;
    flag |= strstr(parameters, "\\Flagged") ? RUMBLE_LETTER_FLAGGED : 0;
    flag |= strstr(parameters, "\\Draft") ? RUMBLE_LETTER_DRAFT : 0;
    flag |= strstr(parameters, "\\Answered") ? RUMBLE_LETTER_ANSWERED : 0;

    /*
     * Process the letters ;
     * For each range, set the message stuff
     */
    parts = rumble_read_words(parameters);
    if (parts->argc > 1) {
        rumble_scan_ranges(&ranges, parts->argv[0]);
        for (x = 0; ranges[x].start != 0; x++) {
            first = ranges[x].start;
            last = ranges[x].end;
            a = 0;
            printf("Storing flags for letter %lu through %u\n", first, last);
            foreach((rumble_letter *), letter, folder->letters, iter) {
                a++;
                if (useUID && (letter->id < first || (last > 0 && letter->id > last))) continue;
                if (!useUID && (a < first || (last > 0 && a > last))) continue;

                /* +FLAGS ? */
                if (control == 1) letter->flags |= flag;

                /* -FLAGS ? */
                if (control == -1) letter->flags -= (flag & letter->flags);

                /* FLAGS ? */
                if (control == 0) letter->flags = (flag | RUMBLE_LETTER_UNREAD);
                if (!silent) {
                    rcprintf(session, "* %u FETCH (FLAGS (%s%s%s%s))\r\n", (a + 1), (letter->flags == RUMBLE_LETTER_RECENT) ? "\\Recent " : "",
                             (letter->flags & RUMBLE_LETTER_READ) ? "\\Seen " : "", (letter->flags & RUMBLE_LETTER_DELETED) ? "\\Deleted " : "",
                             (letter->flags & RUMBLE_LETTER_FLAGGED) ? "\\Flagged " : "");
                }

                /*
                 * printf("Set flags for letter %"PRIu64 " to %08x\n", letter->id, letter->flags);
                 */
            }
        }
    }

    rumble_args_free(parts);
    rcprintf(session, "%s OK STORE completed\r\n", extra_data);
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    COPY
 =======================================================================================================================
 */
ssize_t rumble_server_imap_copy(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    size_t                          first,
                                    last,
                                    a,
                                    x,
                                    useUID;
    rumble_mailman_shared_folder    *destination;
    rumble_letter                   *letter;
    rumble_args                     *parts;
    rangePair                       ranges[64];
    char                            folderName[100];
    /* Check for selected folder */
    rumble_mailman_shared_folder    *folder;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    d_iterator                      iter;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/


    /* Is a folder selected to copy from? */
    folder = rumble_mailman_current_folder(imap);
    if (!folder) {
        rcprintf(session, "%s NO COPY: I don't know where to copy from!\r\n", extra_data);
        return (RUMBLE_RETURN_IGNORE);
    }

    destination = 0;

    /* Are we using UIDs? */
    useUID = session->flags & rumble_mailman_HAS_UID;

    /* Get the destination folder */
    memset(folderName, 0, 100);
    parts = rumble_read_words(parameters);
    if (parts->argc >= 2) {
        a = strlen(parts->argv[parts->argc - 1]);
        strncpy(folderName, parts->argv[parts->argc - 1], a < 100 ? a : 99);
    }

    /* Check if folder exists */
    destination = rumble_mailman_get_folder(imap, folderName);
    if (!destination) {
        rcprintf(session, "%s NO COPY [TRYCREATE] failed: Destination folder doesn't exist!\r\n", extra_data);
        return (RUMBLE_RETURN_IGNORE);
    }

    /* For each range, copy the messages */
    folder = rumble_mailman_current_folder(imap);   /* originating folder */
    rumble_scan_ranges(&ranges, parts->argv[0]);
    rumble_args_free(parts);
    for (x = 0; ranges[x].start != 0; x++) {
        first = ranges[x].start;
        last = ranges[x].end;
        a = 0;
        foreach((rumble_letter *), letter, folder->letters, iter) {
            a++;
            if (useUID && (letter->id < first || (last > 0 && letter->id > last))) continue;
            if (!useUID && (a < first || (last > 0 && a > last))) continue;
            rumble_mailman_copy_letter(imap->account, letter, destination);
        }
    }

    rcprintf(session, "%s OK COPY completed\r\n", extra_data);
    return (RUMBLE_RETURN_IGNORE);
}

/*
 =======================================================================================================================
    IDLE
 =======================================================================================================================
 */
ssize_t rumble_server_imap_idle(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
    char                            *line;
    char                            buffer[5];
    int                             rc = -1;
    int                             cc = 0;
    struct timeval                  timeout;
    int                             exists = 0;
    int                             recent = 0;
    int                             first = 0;
    int                             oexists = 0;
    int                             orecent = 0;
    int                             ofirst = 0;
#ifdef RUMBLE_MSC
    u_long                          iMode = 1;
#endif
    rumble_letter                   *letter;
    accountSession                  *imap = (accountSession *) session->_svcHandle;
    d_iterator                      iter;
    rumble_mailman_shared_folder    *folder = 0;
    /*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

    folder = rumble_mailman_current_folder(imap);
    if (!folder) {
        rcprintf(session, "%s NO No mailbox selected for fetching!\r\n", extra_data);
        return (RUMBLE_RETURN_IGNORE);
    }

    rcprintf(session, "%s OK IDLE Starting idle mode.\r\n", extra_data);
    memset(buffer, 0, 5);

    /* Retrieve the statistics of the folder before idling */
    rumble_rw_start_read(imap->bag->rrw);
    foreach((rumble_letter *), letter, folder->letters, iter) {
        oexists++;
        if (!ofirst && ((letter->flags & RUMBLE_LETTER_UNREAD) || (letter->flags == RUMBLE_LETTER_RECENT))) ofirst = oexists;
        if (letter->flags == RUMBLE_LETTER_RECENT) orecent++;
    }

    rumble_rw_stop_read(imap->bag->rrw);

    /* While idle, check for stuff, otherwise break off */
    while (rc < 0) {
        timeout.tv_sec = 2;
#ifdef RUMBLE_MSC
        ioctlsocket(session->client->socket, FIONBIO, &iMode);
        rc = recv(session->client->socket, buffer, 1, MSG_PEEK);
        iMode = 0;
        ioctlsocket(session->client->socket, FIONBIO, &iMode);
#else
        rc = recv(session->client->socket, buffer, 1, MSG_PEEK | MSG_DONTWAIT);
#endif
        if (rc == 1) break; /* got data from client again */
        else if (rc == 0)
            return (RUMBLE_RETURN_FAILURE); /* disconnected? */
        else if (rc == -1) {
            cc++;
            if (cc == 10) {

                /* Check the DB for new messages every 40 seconds. */
                rumble_mailman_scan_incoming(folder);
                cc = 0;
            }

            rumble_rw_start_read(imap->bag->rrw);
            foreach((rumble_letter *), letter, folder->letters, iter) {
                exists++;
                if (!first && ((letter->flags & RUMBLE_LETTER_UNREAD) || (letter->flags == RUMBLE_LETTER_RECENT))) first = exists;
                if (letter->flags == RUMBLE_LETTER_RECENT) recent++;
            }

            rumble_rw_stop_read(imap->bag->rrw);
            if (oexists != exists) {
                rcprintf(session, "* %u EXISTS\r\n", exists);
                oexists = exists;
            }

            if (recent != orecent) {
                rcprintf(session, "* %u RECENT\r\n", exists);
                orecent = recent;
            }

            exists = 0;
            recent = 0;
            first = 0;
            sleep(3);
        }
    }

    line = rcread(session);
    if (!line) return (RUMBLE_RETURN_FAILURE);
    else {
        free(line);
        rcprintf(session, "%s OK IDLE completed.\r\n", extra_data);
        return (RUMBLE_RETURN_IGNORE);
    }
}

/*
 =======================================================================================================================
 =======================================================================================================================
 */
ssize_t rumble_server_imap_logout(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {
    return (RUMBLE_RETURN_FAILURE);
}

/*
 =======================================================================================================================
    TESTING
 =======================================================================================================================
 */
ssize_t rumble_server_imap_test(masterHandle *master, sessionHandle *session, const char *parameters, const char *extra_data) {

    /*~~~~~~~~~~~~~~~~~~~*/
    int         x = 0;
    rangePair   ranges[64];
    /*~~~~~~~~~~~~~~~~~~~*/

    rcprintf(session, "<%s>\r\n", parameters);
    rumble_scan_ranges(&ranges, parameters);
    while (1) {
        if (!ranges[x].start) break;
        printf("start: %lu, stop: %lu\n", ranges[x].start, ranges[x].end);
        x++;
    }
}
