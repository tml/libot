#include "client.h"

static void free_anticipated(ot_client* client) {
    if (client->anticipated_needs_free) {
        ot_free_op(client->anticipated);
        client->anticipated_needs_free = false;
    }
}

static void free_buffer(ot_client* client) {
    if (client->buffer_needs_free) {
        ot_free_op(client->buffer);
        client->buffer = NULL;
        client->buffer_needs_free = false;
    }
}

static ot_err buffer_op(ot_client* client, ot_op* op) {
    if (client->buffer == NULL) {
        client->buffer = op;

        // Don't free the buffer because it points to an op in the document.
        client->buffer_needs_free = false;
        return OT_ERR_NONE;
    }

    ot_op* composed = ot_compose(client->buffer, op);
    if (composed == NULL) {
        char* enc = ot_encode(op);
        fprintf(stderr, "Client couldn't add op to the buffer: %s\n", enc);
        free(enc);
        return OT_ERR_BUFFER_FAILED;
    }

    free_buffer(client);

    // Set the buffer and mark it as freeable because it doesn't point to
    // anywhere within the doc.
    client->buffer = composed;
    client->buffer_needs_free = true;

    char* enc = ot_encode(composed);
    fprintf(stderr, "Client's buffer is now: %s\n", enc);
    free(enc);

    return 0;
}

static void send_buffer(ot_client* client) {
    if (client->buffer == NULL) {
        return;
    }

    char* enc_buf = ot_encode(client->buffer);
    client->send(enc_buf);

    free_buffer(client);
    client->ack_required = true;

    // Don't free the anticipated op because it points to an op in the document.
    client->anticipated = client->buffer;
    client->anticipated_needs_free = false;
}

static void fire_op_event(ot_client* client, ot_op* op) {
    assert(client);
    assert(op);
}

ot_client* ot_new_client(send_func send, ot_event_func event, uint32_t id) {
    ot_client* client = malloc(sizeof(ot_client));
    client->buffer = NULL;
    client->anticipated = NULL;
    client->send = send;
    client->event = event;
    client->doc = NULL;
    client->client_id = id;
    client->ack_required = false;
    client->anticipated_needs_free = false;
    client->buffer_needs_free = false;

    return client;
}

void ot_free_client(ot_client* client) {
    ot_doc* doc = client->doc;
    if (doc != NULL) {
        ot_free_doc(client->doc);
    }

    free_anticipated(client);
    free_buffer(client);
    free(client);
}

void ot_client_open(ot_client* client, ot_doc* doc) { client->doc = doc; }

void ot_client_receive(ot_client* client, const char* op) {
    fprintf(stderr, "Client received op: %s\n", op);

    ot_op* dec = ot_new_op(0, "");
    ot_decode(dec, op);
    if (dec->client_id == client->client_id) {
        char hex[41];
        atohex((char*)&hex, (char*)&dec->hash, 20);
        fprintf(stderr, "Op %s was acknowledged.\n", hex);
        ot_free_op(dec);

        client->ack_required = false;
        send_buffer(client);
        return;
    }

    ot_xform_pair p = ot_xform(client->anticipated, dec);
    free_anticipated(client);
    client->anticipated = p.op1_prime;
    client->anticipated_needs_free = true;

    ot_xform_pair p2 = ot_xform(client->buffer, p.op2_prime);
    ot_free_op(client->buffer);
    ot_free_op(p.op2_prime);
    client->buffer = p2.op1_prime;

    if (client->doc == NULL) {
        client->doc = ot_new_doc();
    }
    ot_doc_append(client->doc, &p2.op2_prime);
    fire_op_event(client, p2.op2_prime);
}

ot_err ot_client_apply(ot_client* client, ot_op** op) {
    if (client->doc == NULL) {
        client->doc = ot_new_doc();
    }

    ot_doc* doc = client->doc;
    ot_err append_err = ot_doc_append(doc, op);
    if (append_err != OT_ERR_NONE) {
        return append_err;
    }

    ot_err buf_err = buffer_op(client, *op);
    if (buf_err != OT_ERR_NONE) {
        return buf_err;
    }

    if (!client->ack_required) {
        send_buffer(client);
    }

    return 0;
}
