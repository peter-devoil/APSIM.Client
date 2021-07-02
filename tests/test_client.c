#include <check.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <pthread.h>

#include "replacement.h"
#include "client.h"
#include "client-private.h"

//tmp
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
//endtmp
int server_socket, client_socket, connection_socket;
char* pipe_file;

void setup() {
    server_socket = -1;
    client_socket = -1;
    connection_socket = -1;
    pipe_file = "";
}

// The teardown method closes any open FDs and removes the pipe file
// from the filesystem. This MUST be run after all tests, but it is not
// called automatically if a test fails, so it must be called manually
// before a failed assertion occurs.
void teardown() {
    if (connection_socket != -1) {
        int err = close(connection_socket);
        if (err != 0) {
            printf("Error closing connection socket: %s\n", strerror(errno));
        }
    }
    if (client_socket != -1) {
        int err = close(client_socket);
        if (err != 0) {
            printf("Error closing client socket (%d): %s\n", errno, strerror(errno));
        }
    }
    if (server_socket != -1) {
        int err = close(server_socket);
        if (err != 0) {
            printf("Error closing server socket (%d): %s\n", errno, strerror(errno));
        }
    }
    if (strcmp(pipe_file, "") != 0 && access(pipe_file, F_OK) == 0) {
        remove(pipe_file);
    }
}

void establish_connection() {
    // Create a server and listen for connections.
    server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address;
    address.sun_family = AF_UNIX;
    pipe_file = "/tmp/CoreFxPipe_apsimclient-unittest";
    strcpy(address.sun_path, pipe_file);
    int addr_len = sizeof(address);

    int err = bind(server_socket, (struct sockaddr*)&address, addr_len);
    if (err < 0) {
        printf("bind() failure: %s\n", strerror(errno));
        teardown();
        ck_assert_int_eq(0, err);
    }

    err = listen(server_socket, 10);
    if (err < 0) {
        printf("listen() failure: %s\n", strerror(errno));
        teardown();
        ck_assert_int_eq(0, err);
    }

    // Use the apsim client API to connect to the server.
    client_socket = connectToServer("apsimclient-unittest");

    // Accept the incoming connection from the client.
    connection_socket = accept(server_socket, (struct sockaddr*)&address, (socklen_t*)&addr_len);
    if (connection_socket < 0) {
        printf("accept() failure: %s\n", strerror(errno));
        teardown();
        ck_assert_int_ge(connection_socket, 0);
    }
}

// Read a message from the client, using the expected protocol.
/*
 * Read a message from the client, using the expected protocol.
 * @param msg_length an int* whose value will be set to the message length.
 * @return The message received from the client (must be freed by the caller).
 */
unsigned char* read_from_client(uint32_t* msg_length) {
    char msg_len[4];
    int err = read(connection_socket, msg_len, 4);
    if (err < 0) {
        printf("read() failure: %s\n", strerror(errno));
        teardown();
        ck_assert_int_eq(4, err);
    }

    uint32_t n;
    memcpy(&n, msg_len, 4);

    unsigned char* buf = malloc((n + 1) * sizeof(unsigned char));
    buf[n] = 0;
    err = read(connection_socket, buf, n);
    if (err < 0) {
        printf("read() failure: %s\n", strerror(errno));
        teardown();
        ck_assert_int_eq(n, err);
    }
    *msg_length = n;
    return buf;
}

/**
 * Send a message to the client, using the expected protocol.
 * @param message the message.
 * @return Nothing.
 */
void send_to_client(char* message) {
    size_t len = strlen(message);
    // Send message length.
    int err = send(connection_socket, (char*)&len, 4, 0);
    if (err < 0) {
        printf("send() failure: %s\n", strerror(errno));
        teardown();
        ck_assert_int_eq(4, err);
    }

    // Send the message itself.
    err = send(connection_socket, message, len, 0);
    if (err < 0) {
        printf("send() failure: %s\n", strerror(errno));
        teardown();
        ck_assert_int_eq(len, err);
    }
}

/*
 * Read a Replacement struct from the client, using the expected
 * protocol.
 * @return The replacement struct send by the client.
 */
struct Replacement* read_replacement_from_client() {
    // 1. Read parameter path.
    uint32_t path_len;
    char* path = (char*)read_from_client(&path_len);
    send_to_client("ACK");

    // 2. Read parameter type.
    uint32_t param_type_len;
    char* param_type_bytes = (char*)read_from_client(&param_type_len);
    if (param_type_len != 4) {
        printf("Incorrect param_type_len. Expected 4, got %d", param_type_len);
        teardown();
        ck_assert_int_eq(4, param_type_len);
    }
    int32_t param_type = 0;
    for (uint32_t i = 0; i < 4; i++) {
        param_type += param_type_bytes[i] << i;
    }
    // memcpy(&param_type, param_type_bytes, 4);
    send_to_client("ACK");
    
    uint32_t param_len;
    char* param_value = (char*)read_from_client(&param_len);
    send_to_client("ACK");
    
    struct Replacement* replacement = malloc(sizeof(struct Replacement));
    replacement->path = path;
    replacement->paramType = param_type;
    replacement->value_len = param_len;
    replacement->value = param_value;
    return replacement;
}

/**
 * Assert that two replacement structs are equal.
 */
void assert_replacements_equal(struct Replacement* expected, struct Replacement* actual) {
    if (expected->paramType != actual->paramType) {
        teardown();
        ck_assert_int_eq(expected->paramType, actual->paramType);
    }
    if (strcmp(expected->path, actual->path) != 0) {
        teardown();
        ck_assert_str_eq(expected->path, actual->path);
    }
    if (expected->value_len != actual->value_len) {
        teardown();
        ck_assert_int_eq(expected->value_len, actual->value_len);
    }
    for (uint32_t i = 0; i < expected->value_len; i++) {
        if (expected->value[i] != actual->value[i]) {
            teardown();
            ck_assert_int_eq(expected->value[i], actual->value[i]);
        }
    }
}

/**
 * Use the client API to send a Replacement struct to the server, and ensure
 * that the data transmission follows the replacements protocol.
 */
void test_send_replacement(struct Replacement* change) {
    pthread_t tid;
    pthread_create(&tid, NULL, (void*)read_replacement_from_client, NULL);
    sendReplacementToSocket(client_socket, change);
    struct Replacement* from_client;
    pthread_join(tid, (void**)&from_client);

    assert_replacements_equal(change, from_client);
    replacement_free(from_client);
}

/**
 * Read a RUN command from the client.
 * @param num_changes the number of changes to expect.
 * @return the changes sent by the client.
 */
struct Replacement** read_run_command_from_client(uint32_t* num_changes) {
    // 1. We expect "RUN" from the client.
    uint32_t msg_len;
    char* msg = (char*)read_from_client(&msg_len);
    if (msg_len != 3) {
        teardown();
        ck_assert_int_eq(3, msg_len);
    }
    char* expected = "RUN";
    if (strcmp(expected, msg) != 0) {
        teardown();
        ck_assert_str_eq(expected, msg);
    }
    free(msg);
    send_to_client("ACK");

    // 2. The client will now send through the changes one by one.
    struct Replacement** changes = malloc(*num_changes * sizeof(struct Replacement*));
    for (uint32_t i = 0; i < *num_changes; i++) {
        changes[i] = read_replacement_from_client();
    }

    // 3. The client will now send through a "FIN".
    msg = (char*)read_from_client(&msg_len);
    if (msg_len != 3) {
        teardown();
        ck_assert_int_eq(3, msg_len);
    }
    expected = "FIN";
    if (strcmp(expected, msg) != 0) {
        teardown();
        ck_assert_str_eq(expected, msg);
    }
    free(msg);
    send_to_client("ACK");
    send_to_client("FIN");

    return changes;
}

void run_with_changes(struct Replacement** changes, uint32_t nchanges) {
    pthread_t tid;
    struct Replacement** changes_from_client;
    int err = pthread_create(&tid, NULL, (void*)read_run_command_from_client, &nchanges);
    if (err != 0) {
        printf("pthread_join() error: %s\n", strerror(errno));
        teardown();
        ck_assert_int_eq(err, 0);
    }
    runWithChanges(client_socket, changes, nchanges);
    err = pthread_join(tid, (void**)&changes_from_client);
    if (err != 0) {
        printf("pthread_join() error: %s\n", strerror(errno));
        teardown();
        ck_assert_int_eq(err, 0);
    }

    for (uint32_t i = 0; i < nchanges; i++) {
        assert_replacements_equal(changes[i], changes_from_client[i]);
        replacement_free(changes_from_client[i]);
    }
}

START_TEST(test_connect_to_server) {
    establish_connection();

    char msg[6] = "hello";
    int err = send(client_socket, msg, 5, 0);
    if (err < 0) {
        printf("send() error: %s\n", strerror(errno));
    }
    ck_assert_int_eq(err, 5);

    char incoming[6];
    err = read(connection_socket, (void*)incoming, 5);
    if (err < 0) {
        printf("read() error: %s\n", strerror(errno));
    }
    ck_assert_int_eq(err, 5);
    ck_assert_str_eq(msg, incoming);
}
END_TEST

START_TEST(test_disconnect_from_server) {
    establish_connection();
    disconnectFromServer(client_socket);
    int res = write(client_socket, "x", 1);
    if (res >= 0) {
        teardown();
        ck_assert_int_ge(res, 0);
    }
    if (errno != EBADF) {
        teardown();
        ck_assert_int_eq(EBADF, errno);
    }
    // Prevent the teardown function from trying to close the client
    // socket for a second time.
    client_socket = -1;
}
END_TEST

START_TEST(test_send_to_server) {
    establish_connection();
    int message_length;
    pthread_t tid;
    int err = pthread_create(&tid, NULL, (void*)read_from_client, &message_length);
    if (err != 0) {
        printf("pthread_create() failure: %s\n", strerror(errno));
        teardown();
        ck_assert_int_eq(0, err);
    }
    char* message = "hello, there";
    int expected_length = strlen(message);
    sendToSocket(client_socket, message, expected_length);
    char* from_client;
    pthread_join(tid, (void**)&from_client);
    if (err != 0) {
        printf("pthread_create() failure: %s\n", strerror(errno));
        teardown();
        ck_assert_int_eq(0, err);
    }
    if (message_length != expected_length) {
        teardown();
        ck_assert_int_eq(expected_length, message_length);
    }
    if (strcmp(message, from_client) != 0) {
        teardown();
        ck_assert_str_eq(message, from_client);
    }
    free(from_client);
}
END_TEST

START_TEST(test_read_from_server) {
    establish_connection();
    char* message_to_client = "this is a message from the server, to the client!";
    pthread_t tid;
    int err = pthread_create(&tid, NULL, (void*)send_to_client, message_to_client);
    if (err != 0) {
        printf("pthread_create() failure: %s\n", strerror(errno));
        teardown();
        ck_assert_int_eq(0, err);
    }

    uint32_t expected_length = strlen(message_to_client);
    uint32_t actual_length;
    char* from_server = readFromSocket(client_socket, &actual_length);
    err = pthread_join(tid, NULL);
    if (err != 0) {
        printf("pthread_create() failure: %s\n", strerror(errno));
        teardown();
        ck_assert_int_eq(0, err);
    }

    if (expected_length != actual_length) {
        teardown();
        ck_assert_int_eq(expected_length, actual_length);
    }
    if (strcmp(message_to_client, from_server) != 0) {
        teardown();
        ck_assert_str_eq(message_to_client, from_server);
    }

    free(from_server);
}
END_TEST

START_TEST(test_send_replacement_to_server) {
    establish_connection();

    uint32_t nchanges = 2;
    struct Replacement* changes[nchanges];
    changes[0] = createIntReplacement("xyz", -65536);
    changes[1] = createDoubleReplacement("[Wheat].Path", -11400000.5);
    for (uint32_t i = 0; i < nchanges; i++) {
        test_send_replacement(changes[i]);
        replacement_free(changes[i]);
    }
}
END_TEST

START_TEST(test_run_with_changes) {
    establish_connection();

    struct Replacement* change = createDoubleReplacement("xyz", 12.5);
    run_with_changes(&change, 1);
    replacement_free(change);

    change = createIntReplacement("", -65536);
    run_with_changes(&change, 1);
    replacement_free(change);

    int nchanges = 10;
    struct Replacement** changes = malloc(nchanges * sizeof(struct Replacement*));
    for (uint32_t i = 0; i < nchanges; i++) {
        char path[11];
        sprintf(path, "change[%u]", i);
        changes[i] = createIntReplacement(path, i);
    }
    run_with_changes(changes, nchanges);
    for (uint32_t i = 0; i < nchanges; i++) {
        replacement_free(changes[i]);
    }
}
END_TEST

Suite* client_test_suite() {
    Suite* suite;
    TCase* test_case;

    suite = suite_create("Client Tests");
    test_case = tcase_create("Client Test Case");

    tcase_add_test(test_case, test_connect_to_server);
    tcase_add_test(test_case, test_disconnect_from_server);
    tcase_add_test(test_case, test_send_to_server);
    tcase_add_test(test_case, test_read_from_server);
    tcase_add_test(test_case, test_send_replacement_to_server);
    tcase_add_test(test_case, test_run_with_changes);
    tcase_add_checked_fixture(test_case, setup, teardown);

    suite_add_tcase(suite, test_case);
    return suite;
}