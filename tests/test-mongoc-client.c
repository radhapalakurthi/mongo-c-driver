#include <fcntl.h>
#include <mongoc.h>

#include "mongoc-client-private.h"
#include "mongoc-cursor-private.h"
#include "mongoc-uri-private.h"

#include "TestSuite.h"
#include "test-conveniences.h"
#include "test-libmongoc.h"
#include "mock_server/future.h"
#include "mock_server/future-functions.h"
#include "mock_server/mock-server.h"
#include "mongoc-tests.h"

#ifdef _WIN32
# define strcasecmp _stricmp
#endif


#undef MONGOC_LOG_DOMAIN
#define MONGOC_LOG_DOMAIN "client-test"

static mongoc_collection_t *
get_test_collection (mongoc_client_t *client,
                     const char      *name)
{
   mongoc_collection_t *ret;
   char *str;

   str = gen_collection_name (name);
   ret = mongoc_client_get_collection (client, "test", str);
   bson_free (str);

   return ret;
}


static char *
gen_test_user (void)
{
   return bson_strdup_printf ("testuser_%u_%u",
                              (unsigned)time(NULL),
                              (unsigned)gettestpid());
}


static char *
gen_good_uri (const char *username,
              const char *dbname)
{
   char *host = test_framework_get_host ();
   uint16_t port = test_framework_get_port ();
   char *uri = bson_strdup_printf ("mongodb://%s:testpass@%s:%hu/%s",
                                   username,
                                   host,
                                   port,
                                   dbname);

   bson_free (host);
   return uri;
}


static void
test_mongoc_client_authenticate (void *context)
{
   mongoc_client_t *admin_client;
   char *username;
   char *uri;
   bson_t roles;
   mongoc_database_t *database;
   char *uri_str_no_auth;
   char *uri_str_auth;
   mongoc_collection_t *collection;
   mongoc_client_t *auth_client;
   mongoc_cursor_t *cursor;
   const bson_t *doc;
   bson_error_t error;
   bool r;
   bson_t q;

   /*
    * Log in as admin.
    */
   admin_client = test_framework_client_new ();

   /*
    * Add a user to the test database.
    */
   username = gen_test_user ();
   uri = gen_good_uri (username, "test");

   database = mongoc_client_get_database (admin_client, "test");
   mongoc_database_remove_user (database, username, &error);
   bson_init (&roles);
   BCON_APPEND (&roles,
                "0", "{", "role", "read", "db", "test", "}");

   ASSERT_OR_PRINT (mongoc_database_add_user(database, username, "testpass",
                                             &roles, NULL, &error), error);

   mongoc_database_destroy(database);

   /*
    * Try authenticating with that user.
    */
   bson_init(&q);
   uri_str_no_auth = test_framework_get_uri_str_no_auth ("test");
   uri_str_auth = test_framework_add_user_password (uri_str_no_auth,
                                                    username,
                                                    "testpass");
   auth_client = mongoc_client_new (uri_str_auth);
   collection = mongoc_client_get_collection (auth_client, "test", "test");
   cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 1, 0,
                                   &q, NULL, NULL);
   r = mongoc_cursor_next(cursor, &doc);
   if (!r) {
      r = mongoc_cursor_error(cursor, &error);
      if (r) {
         printf("Authentication failure: \"%s\"", error.message);
      }
      assert(!r);
   }

   /*
    * Remove all test users.
    */
   database = mongoc_client_get_database (admin_client, "test");
   r = mongoc_database_remove_all_users (database, &error);
   assert (r);

   mongoc_cursor_destroy (cursor);
   mongoc_collection_destroy (collection);
   bson_free (uri_str_no_auth);
   bson_free (uri_str_auth);
   mongoc_client_destroy (auth_client);
   bson_destroy (&roles);
   bson_free (uri);
   bson_free (username);
   mongoc_database_destroy (database);
   mongoc_client_destroy (admin_client);
}


static int should_run_auth_tests (void)
{
   char *user;
#ifndef MONGOC_ENABLE_SSL
   mongoc_client_t *client = test_framework_client_new ();
   uint32_t server_id = mongoc_cluster_preselect(&client->cluster, MONGOC_OPCODE_QUERY, NULL, NULL);

   if (mongoc_cluster_node_max_wire_version (&client->cluster, server_id) > 2) {
      mongoc_client_destroy (client);
      return 0;
   }
#endif

   /* run auth tests if the MONGOC_TEST_USER env var is set */
   user = test_framework_get_admin_user ();
   bson_free (user);
   return user ? 1 : 0;
}


int skip_if_mongos (void)
{
   return test_framework_is_mongos () ? 0 : 1;
}


static void
test_mongoc_client_authenticate_failure (void *context)
{
   mongoc_collection_t *collection;
   mongoc_cursor_t *cursor;
   mongoc_client_t *client;
   const bson_t *doc;
   bson_error_t error;
   bool r;
   bson_t q;
   bson_t empty = BSON_INITIALIZER;
   char *host = test_framework_get_host ();
   char *uri_str_no_auth = test_framework_get_uri_str_no_auth (NULL);
   char *bad_uri_str = test_framework_add_user_password (uri_str_no_auth,
                                                         "baduser",
                                                         "badpass");

   /*
    * Try authenticating with bad user.
    */
   bson_init(&q);
   client = mongoc_client_new (bad_uri_str);

   collection = mongoc_client_get_collection(client, "test", "test");
   suppress_one_message ();
   cursor = mongoc_collection_find(collection, MONGOC_QUERY_NONE, 0, 1, 0,
                                   &q, NULL, NULL);
   suppress_one_message ();
   r = mongoc_cursor_next(cursor, &doc);
   assert(!r);
   r = mongoc_cursor_error(cursor, &error);
   assert(r);
   assert(error.domain == MONGOC_ERROR_CLIENT);
   assert(error.code == MONGOC_ERROR_CLIENT_AUTHENTICATE);
   mongoc_cursor_destroy(cursor);

   /*
    * Try various commands while in the failed state to ensure we get the
    * same sort of errors.
    */
   suppress_one_message ();
   suppress_one_message ();
   suppress_one_message ();
   r = mongoc_collection_insert (collection, MONGOC_INSERT_NONE,
                                 &empty, NULL, &error);
   assert (!r);
   assert (error.domain == MONGOC_ERROR_CLIENT);
   assert (error.code == MONGOC_ERROR_CLIENT_AUTHENTICATE);

   /*
    * Try various commands while in the failed state to ensure we get the
    * same sort of errors.
    */
   suppress_one_message ();
   suppress_one_message ();
   suppress_one_message ();
   r = mongoc_collection_update (collection, MONGOC_UPDATE_NONE,
                                 &q, &empty, NULL, &error);
   assert (!r);
   assert (error.domain == MONGOC_ERROR_CLIENT);
   assert (error.code == MONGOC_ERROR_CLIENT_AUTHENTICATE);

   bson_free (host);
   bson_free (uri_str_no_auth);
   bson_free (bad_uri_str);
   mongoc_collection_destroy(collection);
   mongoc_client_destroy(client);
}


#ifdef TODO_CDRIVER_689
static void
test_wire_version (void)
{
   mongoc_collection_t *collection;
   mongoc_cursor_t *cursor;
   mongoc_client_t *client;
   mock_server_t *server;
   const bson_t *doc;
   bson_error_t error;
   bool r;
   bson_t q = BSON_INITIALIZER;

   server = mock_server_new ();
   mock_server_auto_ismaster (server, "{'ok': 1.0,"
                                       " 'ismaster': true,"
                                       " 'minWireVersion': 10,"
                                       " 'maxWireVersion': 11}");

   mock_server_run (server);

   client = mongoc_client_new_from_uri (mock_server_get_uri (server));

   collection = mongoc_client_get_collection (client, "test", "test");

   cursor = mongoc_collection_find (collection,
                                    MONGOC_QUERY_NONE,
                                    0,
                                    1,
                                    0,
                                    &q,
                                    NULL,
                                    NULL);

   r = mongoc_cursor_next (cursor, &doc);
   assert (!r);

   r = mongoc_cursor_error (cursor, &error);
   assert (r);

   assert (error.domain == MONGOC_ERROR_PROTOCOL);
   assert (error.code == MONGOC_ERROR_PROTOCOL_BAD_WIRE_VERSION);

   mongoc_cursor_destroy (cursor);
   mongoc_collection_destroy (collection);
   mongoc_client_destroy (client);
   mock_server_destroy (server);
}
#endif


static void
test_mongoc_client_read_prefs (void)
{
   mongoc_collection_t *collection;
   mongoc_read_prefs_t *read_prefs;
   mongoc_cursor_t *cursor;
   mongoc_client_t *client;
   mock_server_t *server;
   const bson_t *doc;
   bson_t b = BSON_INITIALIZER;
   bson_t q = BSON_INITIALIZER;
   future_t *future;
   request_t *request;

   server = mock_server_new ();
   mock_server_auto_ismaster (server, "{'ok': 1,"
                                       " 'ismaster': true,"
                                       " 'msg': 'isdbgrid'}");
   mock_server_run (server);
   client = mongoc_client_new_from_uri (mock_server_get_uri (server));

   collection = mongoc_client_get_collection (client, "test", "test");

   bson_append_utf8 (&b, "dc", 2, "ny", 2);

   read_prefs = mongoc_read_prefs_new (MONGOC_READ_SECONDARY_PREFERRED);
   mongoc_read_prefs_add_tag (read_prefs, &b);
   mongoc_read_prefs_add_tag (read_prefs, NULL);
   mongoc_collection_set_read_prefs (collection, read_prefs);

   cursor = mongoc_collection_find (collection,
                                    MONGOC_QUERY_NONE,
                                    0,
                                    1,
                                    0,
                                    &q,
                                    NULL,
                                    read_prefs);

   future = future_cursor_next (cursor, &doc);

   request = mock_server_receives_query (
         server,
         "test.test",
         MONGOC_QUERY_NONE,
         0,
         0,
         "{'$query': {},"
         " '$readPreference': {'mode': 'secondaryPreferred',"
         "                     'tags': [{'dc': 'ny'}, {}]}}",
         NULL);

   mock_server_replies (request,
                        0,                    /* flags */
                        0,                    /* cursorId */
                        0,                    /* startingFrom */
                        1,                    /* numberReturned */
                        "{'a': 1}");

   /* mongoc_cursor_next returned true */
   assert (future_get_bool (future));

   request_destroy (request);
   future_destroy (future);
   mongoc_read_prefs_destroy (read_prefs);
   mongoc_cursor_destroy (cursor);
   mongoc_collection_destroy (collection);
   mongoc_client_destroy (client);
   mock_server_destroy (server);
   bson_destroy (&b);
}


static void
test_mongoc_client_command (void)
{
   mongoc_client_t *client;
   mongoc_cursor_t *cursor;
   const bson_t *doc;
   bool r;
   bson_t cmd = BSON_INITIALIZER;

   client = test_framework_client_new ();
   assert (client);

   bson_append_int32 (&cmd, "ping", 4, 1);

   cursor = mongoc_client_command (client, "admin", MONGOC_QUERY_NONE, 0, 1, 0, &cmd, NULL, NULL);
   assert (!cursor->redir_primary);

   r = mongoc_cursor_next (cursor, &doc);
   assert (r);
   assert (doc);

   r = mongoc_cursor_next (cursor, &doc);
   assert (!r);
   assert (!doc);

   mongoc_cursor_destroy (cursor);
   mongoc_client_destroy (client);
   bson_destroy (&cmd);
}


static void
test_mongoc_client_command_secondary (void)
{
   mongoc_client_t *client;
   mongoc_cursor_t *cursor;
   mongoc_read_prefs_t *read_prefs;
   bson_t cmd = BSON_INITIALIZER;

   client = test_framework_client_new ();
   assert (client);

   BSON_APPEND_INT32 (&cmd, "invalid_command_here", 1);

   read_prefs = mongoc_read_prefs_new (MONGOC_READ_PRIMARY_PREFERRED);

   suppress_one_message ();
   cursor = mongoc_client_command (client, "admin", MONGOC_QUERY_NONE, 0, 1, 0, &cmd, NULL, read_prefs);

   mongoc_read_prefs_destroy (read_prefs);

   /* ensure we detected this must go to primary */
   assert (cursor->redir_primary);

   mongoc_cursor_destroy (cursor);
   mongoc_client_destroy (client);
   bson_destroy (&cmd);
}

static void
test_mongoc_client_preselect (void)
{
   mongoc_client_t *client;
   bson_error_t error;

   client = test_framework_client_new ();
   assert (client);

   ASSERT_OR_PRINT (_mongoc_client_preselect (client, MONGOC_OPCODE_INSERT,
                                              NULL, NULL, &error), error);

   mongoc_client_destroy (client);
}


static void
test_unavailable_seeds (void)
{
   mock_server_t *servers[2];
   char **uri_strs;
   char **uri_str;
   mongoc_client_t *client;
   mongoc_collection_t *collection;
   mongoc_cursor_t *cursor;
   bson_t query = BSON_INITIALIZER;
   const bson_t *doc;
   bson_error_t error;
   
   int i;

   for (i = 0; i < 2; i++) {
      servers[i] = mock_server_down ();  /* hangs up on all requests */
      mock_server_run (servers[i]);
   }
   
   uri_str = uri_strs = bson_malloc0 (7 * sizeof (char *));
   *(uri_str++) = bson_strdup_printf (
      "mongodb://%s",
      mock_server_get_host_and_port (servers[0]));

   *(uri_str++) = bson_strdup_printf (
      "mongodb://%s,%s",
      mock_server_get_host_and_port (servers[0]),
      mock_server_get_host_and_port (servers[1]));

   *(uri_str++) = bson_strdup_printf (
      "mongodb://%s,%s/?replicaSet=rs",
      mock_server_get_host_and_port (servers[0]),
      mock_server_get_host_and_port (servers[1]));

   *(uri_str++) = bson_strdup_printf (
      "mongodb://u:p@%s",
      mock_server_get_host_and_port (servers[0]));

   *(uri_str++) = bson_strdup_printf (
      "mongodb://u:p@%s,%s",
      mock_server_get_host_and_port (servers[0]),
      mock_server_get_host_and_port (servers[1]));

   *(uri_str++) = bson_strdup_printf (
      "mongodb://u:p@%s,%s/?replicaSet=rs",
      mock_server_get_host_and_port (servers[0]),
      mock_server_get_host_and_port (servers[1]));

   for (i = 0; i < (sizeof(uri_strs) / sizeof(const char *)); i++) {
      client = mongoc_client_new (uri_strs[i]);
      assert (client);

      collection = mongoc_client_get_collection (client, "test", "test");
      cursor = mongoc_collection_find (collection,
                                       MONGOC_QUERY_NONE,
                                       0,
                                       0,
                                       0,
                                       &query,
                                       NULL,
                                       NULL);

      assert (! mongoc_cursor_next (cursor, &doc));
      assert (mongoc_cursor_error (cursor, &error));
      ASSERT_CMPINT (error.domain, ==, MONGOC_ERROR_SERVER_SELECTION);
      ASSERT_CMPINT (error.code, ==, MONGOC_ERROR_SERVER_SELECTION_FAILURE);

      mongoc_cursor_destroy (cursor);
      mongoc_collection_destroy (collection);
      mongoc_client_destroy (client);
   }

   for (i = 0; i < 2; i++) {
      mock_server_destroy (servers[i]);
   }

   bson_strfreev (uri_strs);
}


typedef enum {
   NO_CONNECT,
   CONNECT,
   RECONNECT
} connection_option_t;


static bool
responder (request_t *request,
           void *data)
{
   if (!strcmp (request->command_name, "foo")) {
      mock_server_replies_simple (request, "{'ok': 1}");
      request_destroy (request);
      return true;
   }

   return false;
}


/* mongoc_set_for_each callback */
static bool
host_equals (void *item,
             void *ctx)
{
   mongoc_server_description_t *sd;
   const char *host_and_port;

   sd = (mongoc_server_description_t *) item;
   host_and_port = (const char *) ctx;

   return !strcasecmp (sd->host.host_and_port, host_and_port);
}


/* CDRIVER-721 catch errors in _mongoc_cluster_destroy */
static void 
test_seed_list (bool rs,
                connection_option_t connection_option,
                bool pooled)
{
   mock_server_t *server;
   mock_server_t *down_servers[3];
   int i;
   char *uri_str;
   mongoc_uri_t *uri;
   mongoc_client_pool_t *pool = NULL;
   mongoc_client_t *client;
   mongoc_topology_t *topology;
   mongoc_topology_description_t *td;
   mongoc_read_prefs_t *primary_pref;
   uint32_t discovered_nodes_len;
   bson_t reply;
   bson_error_t error;
   uint32_t id;

   server = mock_server_new ();
   mock_server_run (server);

   for (i = 0; i < 3; i++) {
      down_servers[i] = mock_server_down ();
      mock_server_run (down_servers[i]);
   }

   uri_str = bson_strdup_printf (
      "mongodb://%s,%s,%s,%s",
      mock_server_get_host_and_port (server),
      mock_server_get_host_and_port (down_servers[0]),
      mock_server_get_host_and_port (down_servers[1]),
      mock_server_get_host_and_port (down_servers[2]));

   uri = mongoc_uri_new (uri_str);
   assert (uri);

   if (rs) {
      mock_server_auto_ismaster (server,
                                 "{'ok': 1,"
                                 " 'ismaster': true,"
                                 " 'setName': 'rs',"
                                 " 'hosts': ['%s']}",
                                 mock_server_get_host_and_port (server));

      mongoc_uri_set_option_as_utf8 (uri, "replicaSet", "rs");
   } else {
      mock_server_auto_ismaster (server,
                                 "{'ok': 1,"
                                 " 'ismaster': true,"
                                 " 'msg': 'isdbgrid'}");
   }

   /* auto-respond to "foo" command */
   mock_server_autoresponds (server, responder, NULL, NULL);

   if (pooled) {
      pool = mongoc_client_pool_new (uri);
      client = mongoc_client_pool_pop (pool);
   } else {
      client = mongoc_client_new_from_uri (uri);
   }

   topology = client->topology;
   td = &topology->description;

   if (rs) {
      ASSERT_CMPINT (td->type, ==, MONGOC_TOPOLOGY_RS_NO_PRIMARY);
   } else {
      ASSERT_CMPINT (td->type, ==, MONGOC_TOPOLOGY_UNKNOWN);
   }

   ASSERT_CMPINT (4, ==, (int) td->servers->items_len);

   /* a mongos load-balanced connection never removes down nodes */
   discovered_nodes_len = rs ? 1 : 4;

   primary_pref = mongoc_read_prefs_new (MONGOC_READ_PRIMARY);

   if (connection_option == CONNECT || connection_option == RECONNECT) {
      /* only localhost:port responds to initial discovery. the other seeds are
       * discarded from replica set topology, but remain for sharded. */
      ASSERT_OR_PRINT (mongoc_client_command_simple (
         client, "test", tmp_bson("{'foo': 1}"),
         primary_pref, &reply, &error), error);

      bson_destroy (&reply);

      ASSERT_CMPINT (discovered_nodes_len, ==, (int) td->servers->items_len);

      if (rs) {
         ASSERT_CMPINT (td->type, ==, MONGOC_TOPOLOGY_RS_WITH_PRIMARY);
      } else {
         ASSERT_CMPINT (td->type, ==, MONGOC_TOPOLOGY_SHARDED);
      }

      if (pooled) {
         /* nodes created on demand when we use servers for actual operations */
         ASSERT_CMPINT ((int) client->cluster.nodes->items_len, ==, 1);
      }
   }

   if (connection_option == RECONNECT) {
      id = mongoc_set_find_id (td->servers,
                               host_equals,
                               (void *) mock_server_get_host_and_port (server));
      ASSERT_CMPINT (id, !=, 0);
      mongoc_topology_invalidate_server (topology, id);
      if (rs) {
         ASSERT_CMPINT (td->type, ==, MONGOC_TOPOLOGY_RS_NO_PRIMARY);
      } else {
         ASSERT_CMPINT (td->type, ==, MONGOC_TOPOLOGY_SHARDED);
      }

      /* TODO: CDRIVER-699 shouldn't need to set topology stale */
      topology->stale = true;

      ASSERT_OR_PRINT (mongoc_client_command_simple (
         client, "test", tmp_bson("{'foo': 1}"),
         primary_pref, &reply, &error), error);

      bson_destroy (&reply);

      ASSERT_CMPINT (discovered_nodes_len, ==, (int) td->servers->items_len);

      if (pooled) {
         ASSERT_CMPINT ((int) client->cluster.nodes->items_len, ==, 1);
      }
   }

   /* testing for crashes like CDRIVER-721 */

   if (pooled) {
      mongoc_client_pool_push (pool, client);
      mongoc_client_pool_destroy (pool);
   } else {
      mongoc_client_destroy (client);
   }

   mongoc_read_prefs_destroy (primary_pref);
   mongoc_uri_destroy (uri);
   bson_free (uri_str);

   for (i = 0; i < 3; i++) {
      mock_server_destroy (down_servers[i]);
   }

   mock_server_destroy (server);
}


#ifdef TODO_CDRIVER_789
static void
test_rs_seeds_no_connect_single (void)
{
   test_seed_list (true, NO_CONNECT, false);
}


static void
test_rs_seeds_no_connect_pooled (void)
{
   test_seed_list (true, NO_CONNECT, true);
}


static void
test_rs_seeds_connect_single (void)
{
   test_seed_list (true, CONNECT, false);
}

static void
test_rs_seeds_connect_pooled (void)
{
   test_seed_list (true, CONNECT, true);
}


static void
test_rs_seeds_reconnect_single (void)
{
   test_seed_list (true, RECONNECT, false);
}


static void
test_rs_seeds_reconnect_pooled (void)
{
   test_seed_list (true, RECONNECT, true);
}
#endif


static void
test_mongos_seeds_no_connect_single (void)
{
   test_seed_list (false, NO_CONNECT, false);
}


static void
test_mongos_seeds_no_connect_pooled (void)
{
   test_seed_list (false, NO_CONNECT, true);
}


#ifdef TODO_CDRIVER_789
static void
test_mongos_seeds_connect_single (void)
{
   test_seed_list (false, CONNECT, false);
}


static void
test_mongos_seeds_connect_pooled (void)
{
   test_seed_list (false, CONNECT, true);
}


static void
test_mongos_seeds_reconnect_single (void)
{
   test_seed_list (false, RECONNECT, false);
}


static void
test_mongos_seeds_reconnect_pooled (void)
{
   test_seed_list (false, RECONNECT, true);
}
#endif

static void
test_recovering (void)
{
   mock_server_t *server;
   mongoc_uri_t *uri;
   mongoc_client_t *client;
   mongoc_read_mode_t read_mode;
   mongoc_read_prefs_t *prefs;
   bson_error_t error;

   server = mock_server_new ();
   mock_server_run (server);

   /* server is "recovering": not master, not secondary */
   mock_server_auto_ismaster (server,
                              "{'ok': 1,"
                              " 'ismaster': false,"
                              " 'secondary': false,"
                              " 'setName': 'rs',"
                              " 'hosts': ['%s']}",
                              mock_server_get_host_and_port (server));

   uri = mongoc_uri_copy (mock_server_get_uri (server));
   mongoc_uri_set_option_as_utf8 (uri, "replicaSet", "rs");
   client = mongoc_client_new_from_uri (uri);
   prefs = mongoc_read_prefs_new (MONGOC_READ_PRIMARY);

   /* recovering member matches no read mode */
   for (read_mode = MONGOC_READ_PRIMARY;
        read_mode <= MONGOC_READ_NEAREST;
        read_mode++) {
      mongoc_read_prefs_set_mode (prefs, read_mode);
      assert (!mongoc_cluster_preselect (&client->cluster,
                                         MONGOC_OPCODE_QUERY,
                                         prefs, &error));
   }

   mongoc_read_prefs_destroy (prefs);
   mongoc_client_destroy (client);
   mongoc_uri_destroy (uri);
   mock_server_destroy (server);
}


static void
test_exhaust_cursor (void *context)
{
   mongoc_stream_t *stream;
   mongoc_write_concern_t *wr;
   mongoc_client_t *client;
   mongoc_collection_t *collection;
   mongoc_cursor_t *cursor;
   mongoc_cursor_t *cursor2;
   const bson_t *doc;
   bson_t q;
   bson_t b[10];
   bson_t *bptr[10];
   int i;
   bool r;
   uint32_t hint;
   bson_error_t error;
   bson_oid_t oid;

   client = test_framework_client_new ();
   assert (client);

   collection = get_test_collection (client, "test_exhaust_cursor");
   assert (collection);

   mongoc_collection_drop(collection, &error);

   wr = mongoc_write_concern_new ();
   mongoc_write_concern_set_journal (wr, true);

   /* bulk insert some records to work on */
   {
      bson_init(&q);

      for (i = 0; i < 10; i++) {
         bson_init(&b[i]);
         bson_oid_init(&oid, NULL);
         bson_append_oid(&b[i], "_id", -1, &oid);
         bson_append_int32(&b[i], "n", -1, i % 2);
         bptr[i] = &b[i];
      }

      BEGIN_IGNORE_DEPRECATIONS;
      ASSERT_OR_PRINT (mongoc_collection_insert_bulk (
                          collection, MONGOC_INSERT_NONE,
                          (const bson_t **)bptr, 10, wr, &error),
                       error);
      END_IGNORE_DEPRECATIONS;
   }

   /* create a couple of cursors */
   {
      cursor = mongoc_collection_find (collection, MONGOC_QUERY_EXHAUST, 0, 0, 0, &q,
                                       NULL, NULL);

      cursor2 = mongoc_collection_find (collection, MONGOC_QUERY_NONE, 0, 0, 0, &q,
                                        NULL, NULL);
   }

   /* Read from the exhaust cursor, ensure that we're in exhaust where we
    * should be and ensure that an early destroy properly causes a disconnect
    * */
   {
      uint32_t local_hint;

      r = mongoc_cursor_next (cursor, &doc);
      if (!r) {
         mongoc_cursor_error (cursor, &error);
         printf ("cursor error: %s\n", error.message);
      }
      assert (r);
      assert (doc);
      assert (cursor->in_exhaust);
      assert (client->in_exhaust);
      local_hint = cursor->hint;

      /* destroy the cursor, make sure a disconnect happened */
      mongoc_cursor_destroy (cursor);
      stream = (mongoc_stream_t *)mongoc_set_get(client->cluster.nodes, local_hint);
      assert (! stream);

      assert (! client->in_exhaust);
   }

   /* Grab a new exhaust cursor, then verify that reading from that cursor
    * (putting the client into exhaust), breaks a mid-stream read from a
    * regular cursor */
   {
      cursor = mongoc_collection_find (collection, MONGOC_QUERY_EXHAUST, 0, 0, 0, &q,
                                       NULL, NULL);

      for (i = 0; i < 5; i++) {
         r = mongoc_cursor_next (cursor2, &doc);
         assert (r);
         assert (doc);
      }

      r = mongoc_cursor_next (cursor, &doc);
      assert (r);
      assert (doc);

      doc = NULL;
      r = mongoc_cursor_next (cursor2, &doc);
      assert (!r);
      assert (!doc);

      mongoc_cursor_error(cursor2, &error);
      assert (error.domain == MONGOC_ERROR_CLIENT);
      assert (error.code == MONGOC_ERROR_CLIENT_IN_EXHAUST);

      mongoc_cursor_destroy (cursor2);
   }

   /* make sure writes fail as well */
   {
      BEGIN_IGNORE_DEPRECATIONS;
      r = mongoc_collection_insert_bulk (collection, MONGOC_INSERT_NONE,
                                         (const bson_t **)bptr, 10, wr, &error);
      END_IGNORE_DEPRECATIONS;

      assert (!r);
      assert (error.domain == MONGOC_ERROR_CLIENT);
      assert (error.code == MONGOC_ERROR_CLIENT_IN_EXHAUST);
   }

   /* we're still in exhaust.
    *
    * 1. check that we can create a new cursor, as long as we don't read from it
    * 2. fully exhaust the exhaust cursor
    * 3. make sure that we don't disconnect at destroy
    * 4. make sure we can read the cursor we made during the exhuast
    */
   {
      cursor2 = mongoc_collection_find (collection, MONGOC_QUERY_NONE, 0, 0, 0, &q,
                                        NULL, NULL);

      stream = (mongoc_stream_t *)mongoc_set_get(client->cluster.nodes, cursor->hint);
      hint = cursor->hint;

      for (i = 1; i < 10; i++) {
         r = mongoc_cursor_next (cursor, &doc);
         assert (r);
         assert (doc);
      }

      r = mongoc_cursor_next (cursor, &doc);
      assert (!r);
      assert (!doc);

      mongoc_cursor_destroy (cursor);

      assert (stream == (mongoc_stream_t *)mongoc_set_get(client->cluster.nodes, hint));

      r = mongoc_cursor_next (cursor2, &doc);
      assert (r);
      assert (doc);
   }

   bson_destroy(&q);
   for (i = 0; i < 10; i++) {
      bson_destroy(&b[i]);
   }

   ASSERT_OR_PRINT (mongoc_collection_drop (collection, &error), error);

   mongoc_write_concern_destroy (wr);
   mongoc_cursor_destroy (cursor2);
   mongoc_collection_destroy(collection);
   mongoc_client_destroy (client);
}


static void
test_server_status (void)
{
   mongoc_client_t *client;
   bson_error_t error;
   bson_iter_t iter;
   bson_t reply;

   client = test_framework_client_new ();
   assert (client);

   ASSERT_OR_PRINT (mongoc_client_get_server_status (client, NULL,
                                                     &reply, &error), error);

   assert (bson_iter_init_find (&iter, &reply, "host"));
   assert (bson_iter_init_find (&iter, &reply, "version"));
   assert (bson_iter_init_find (&iter, &reply, "ok"));

   bson_destroy (&reply);

   mongoc_client_destroy (client);
}


static void
test_get_database_names (void)
{
   mock_server_t *server = mock_server_with_autoismaster (0);
   mongoc_client_t *client;
   bson_error_t error;
   future_t *future;
   request_t *request;
   char **names;

   mock_server_run (server);
   client = mongoc_client_new_from_uri (mock_server_get_uri (server));
   future = future_client_get_database_names (client, &error);
   request = mock_server_receives_command (server,
                                            "admin",
                                            MONGOC_QUERY_SLAVE_OK,
                                            "{'listDatabases': 1}");
   mock_server_replies (
         request, 0, 0, 0, 1,
         "{'ok': 1.0, 'databases': [{'name': 'a'}, {'name': 'b'}]}");
   names = future_get_char_ptr_ptr (future);
   assert (!strcmp(names[0], "a"));
   assert (!strcmp(names[1], "b"));
   assert (NULL == names[2]);

   bson_strfreev (names);
   request_destroy (request);
   future_destroy (future);

   future = future_client_get_database_names (client, &error);
   request = mock_server_receives_command (server,
                                            "admin",
                                            MONGOC_QUERY_SLAVE_OK,
                                            "{'listDatabases': 1}");
   mock_server_replies (
         request, 0, 0, 0, 1,
         "{'ok': 0.0, 'code': 17, 'errmsg': 'err'}");

   names = future_get_char_ptr_ptr (future);
   assert (!names);
   ASSERT_CMPINT (MONGOC_ERROR_QUERY, ==, error.domain);
   ASSERT_CMPSTR ("err", error.message);

   request_destroy (request);
   future_destroy (future);
   mongoc_client_destroy (client);
   mock_server_destroy (server);
}


static void
test_mongoc_client_ipv6 (void)
{
   mongoc_client_t *client;
   bson_error_t error;
   bson_iter_t iter;
   bson_t reply;

   client = mongoc_client_new ("mongodb://[::1]/");
   assert (client);

   ASSERT_OR_PRINT (mongoc_client_get_server_status (client, NULL,
                                                     &reply, &error), error);

   assert (bson_iter_init_find (&iter, &reply, "host"));
   assert (bson_iter_init_find (&iter, &reply, "version"));
   assert (bson_iter_init_find (&iter, &reply, "ok"));

   bson_destroy (&reply);

   mongoc_client_destroy (client);
}


void
test_client_install (TestSuite *suite)
{
#ifdef TODO_CDRIVER_689
   TestSuite_Add (suite, "/Client/wire_version", test_wire_version);
#endif
   TestSuite_Add (suite, "/Client/read_prefs", test_mongoc_client_read_prefs);

   if (getenv ("MONGOC_CHECK_IPV6")) {
      /* try to validate ipv6 too */
      TestSuite_Add (suite, "/Client/ipv6", test_mongoc_client_ipv6);
   }

   TestSuite_AddFull (suite, "/Client/authenticate", test_mongoc_client_authenticate, NULL, NULL, should_run_auth_tests);
   TestSuite_AddFull (suite, "/Client/authenticate_failure", test_mongoc_client_authenticate_failure, NULL, NULL, should_run_auth_tests);
   TestSuite_Add (suite, "/Client/command", test_mongoc_client_command);
   TestSuite_Add (suite, "/Client/command_secondary", test_mongoc_client_command_secondary);
   TestSuite_Add (suite, "/Client/preselect", test_mongoc_client_preselect);
   TestSuite_Add (suite, "/Client/unavailable_seeds", test_unavailable_seeds);
#ifdef TODO_CDRIVER_789
   TestSuite_Add (suite, "/Client/rs_seeds_no_connect/single", test_rs_seeds_no_connect_single);
   TestSuite_Add (suite, "/Client/rs_seeds_no_connect/pooled", test_rs_seeds_no_connect_pooled);
   TestSuite_Add (suite, "/Client/rs_seeds_connect/single", test_rs_seeds_connect_single);
   TestSuite_Add (suite, "/Client/rs_seeds_connect/pooled", test_rs_seeds_connect_pooled);
   TestSuite_Add (suite, "/Client/rs_seeds_reconnect/single", test_rs_seeds_reconnect_single);
   TestSuite_Add (suite, "/Client/rs_seeds_reconnect/pooled", test_rs_seeds_reconnect_pooled);
#endif
   TestSuite_Add (suite, "/Client/mongos_seeds_no_connect/single", test_mongos_seeds_no_connect_single);
   TestSuite_Add (suite, "/Client/mongos_seeds_no_connect/pooled", test_mongos_seeds_no_connect_pooled);
#ifdef TODO_CDRIVER_789
   TestSuite_Add (suite, "/Client/mongos_seeds_connect/single", test_mongos_seeds_connect_single);
   TestSuite_Add (suite, "/Client/mongos_seeds_connect/pooled", test_mongos_seeds_connect_pooled);
   TestSuite_Add (suite, "/Client/mongos_seeds_reconnect/single", test_mongos_seeds_reconnect_single);
   TestSuite_Add (suite, "/Client/mongos_seeds_reconnect/pooled", test_mongos_seeds_reconnect_pooled);
#endif
   TestSuite_Add (suite, "/Client/recovering", test_recovering);
   TestSuite_AddFull (suite, "/Client/exhaust_cursor", test_exhaust_cursor, NULL, NULL, skip_if_mongos);
   TestSuite_Add (suite, "/Client/server_status", test_server_status);
   TestSuite_Add (suite, "/Client/database_names", test_get_database_names);

#ifdef TODO_CDRIVER_689
   TestSuite_Add (suite, "/Client/wire_version", test_wire_version);
#endif
}

