//
// Created by Lorenzo Donini on 02/09/16.
//

#include <vector>
#include "RxModule.h"

RxModule::RxModule() {
    to_consume_global_seq = 0;
    received_packets = 0;
    pthread_mutex_init(&rx_mutex, NULL);
    pthread_cond_init(&available_msg_cond, NULL);
    inBuffer = new Buffer(this);
}

RxModule::~RxModule() {
    stopModule();
    pthread_mutex_lock(&rx_mutex);
    delete inBuffer;
    pthread_cond_destroy(&available_msg_cond);

    pthread_mutex_unlock(&rx_mutex);
    pthread_mutex_destroy(&rx_mutex);
    //TODO: implement. Also remember to close all ports
}

void RxModule::startModule(StatusInterface * statusInterface) {
    received_packets = 0;
    active = true;
    this->statusInterface = statusInterface;
}

void RxModule::stopModule() {
    pthread_mutex_lock(&rx_mutex);
    active = false;
    //Signaling any thread that is currently waiting for an incoming message.
    // Such thread should then check the status of the module, in order to avoid waiting forever.
    pthread_cond_signal(&available_msg_cond);
    pthread_mutex_unlock(&rx_mutex);
}

bool RxModule::isActive() {
    pthread_mutex_lock(&rx_mutex);
    bool result = active;
    pthread_mutex_unlock(&rx_mutex);
    return result;
}

/*
 * Callback implementation
 */
void RxModule::handleFrame(bool arq, int channel, const void* data, int length) {
    if (length != MAX_LEN_S3TP_PACKET) {
        //TODO: handle error
    }
    int result = handleReceivedPacket((S3TP_PACKET *)data, (uint8_t )channel);
    //TODO: handle error
}

void RxModule::handleLinkStatus(bool linkStatus) {
    if (statusInterface != NULL) {
        statusInterface->onLinkStatusChanged(linkStatus);
    }
}

int RxModule::openPort(uint8_t port) {
    pthread_mutex_lock(&rx_mutex);
    if (!active) {
        pthread_mutex_unlock(&rx_mutex);
        return MODULE_INACTIVE;
    }
    if (open_ports.find(port) != open_ports.end()) {
        //Port is already open
        pthread_mutex_unlock(&rx_mutex);
        return PORT_ALREADY_OPEN;
    }
    open_ports[port] = 0;
    pthread_mutex_unlock(&rx_mutex);

    return CODE_SUCCESS;
}

int RxModule::closePort(uint8_t port) {
    pthread_mutex_lock(&rx_mutex);
    if (!active) {
        pthread_mutex_unlock(&rx_mutex);
        return MODULE_INACTIVE;
    }
    if (open_ports.find(port) != open_ports.end()) {
        open_ports.erase(port);
        return CODE_SUCCESS;
    }

    pthread_mutex_unlock(&rx_mutex);
    return PORT_ALREADY_CLOSED;
}

bool RxModule::isPortOpen(uint8_t port) {
    pthread_mutex_lock(&rx_mutex);
    bool result = (open_ports.find(port) != open_ports.end() && active);
    pthread_mutex_unlock(&rx_mutex);
    return result;
}

int RxModule::handleReceivedPacket(S3TP_PACKET * packet, uint8_t channel) {
    if (!isActive()) {
        return MODULE_INACTIVE;
    }

    //Checking CRC
    uint16_t check = calc_checksum(packet->pdu, packet->hdr.getPduLength());
    if (check != packet->hdr.crc) {
        LOG_WARN("Wrong CRC");
        return CODE_ERROR_CRC_INVALID;
    }

    if (!isPortOpen(packet->hdr.getPort())) {
        //Dropping packet right away
        return CODE_ERROR_PORT_CLOSED;
    }

    //Copying packet
    S3TP_PACKET * pktCopy = new S3TP_PACKET();
    pktCopy->hdr.seq = packet->hdr.seq;
    pktCopy->hdr.port = packet->hdr.port;
    pktCopy->hdr.pdu_length = packet->hdr.pdu_length;
    pktCopy->hdr.crc = packet->hdr.crc;
    pktCopy->hdr.seq_port = packet->hdr.seq_port;
    memcpy(pktCopy->pdu, packet->pdu, packet->hdr.getPduLength());

    S3TP_PACKET_WRAPPER * wrapper = new S3TP_PACKET_WRAPPER();
    wrapper->channel = channel;
    wrapper->pkt = pktCopy;

    int result = inBuffer->write(wrapper);
    if (result != CODE_SUCCESS) {
        //Something bad happened, couldn't put packet in buffer
        return result;
    }

    std::ostringstream stream;
    stream << "RX: Packet received from SPI to port " << (int)pktCopy->hdr.getPort();
    stream << " -> glob_seq " << (int)pktCopy->hdr.getGlobalSequence();
    stream << ", sub_seq " << (int)pktCopy->hdr.getSubSequence();
    stream << ", port_seq " << (int)pktCopy->hdr.seq_port;
    LOG_DEBUG(stream.str());


    pthread_mutex_lock(&rx_mutex);
    if (isCompleteMessageForPortAvailable(pktCopy->hdr.getPort())) {
        //New message is available, notify
        available_messages[pktCopy->hdr.getPort()] = 1;
        pthread_cond_signal(&available_msg_cond);
    }
    pthread_mutex_unlock(&rx_mutex);

    //This variable doesn't need locking, as it is a purely internal counter
    received_packets++;
    if ((received_packets % 256) == 0) {
        //Reordering window reached, flush queues?!
        //TODO: check if overflowed
    }
    //TODO: copy metadata and seq numbers into respective vars and check if something becomes available

    return CODE_SUCCESS;
}

bool RxModule::isCompleteMessageForPortAvailable(int port) {
    PriorityQueue<S3TP_PACKET_WRAPPER*> * q = inBuffer->getQueue(port);
    q->lock();
    PriorityQueue_node<S3TP_PACKET_WRAPPER*> * node = q->getHead();
    uint8_t fragment = 0;
    while (node != NULL) {
        S3TP_PACKET * pkt = node->element->pkt;
        if (pkt->hdr.seq_port != (current_port_sequence[port] + fragment)) {
            //Packet in queue is not the one with highest priority
            break; //Will return false
        } else if (pkt->hdr.moreFragments() && pkt->hdr.getSubSequence() != fragment) {
            //Current fragment number is not the expected number,
            // i.e. at least one fragment is missing to complete the message
            break; //Will return false
        } else if (!pkt->hdr.moreFragments() && pkt->hdr.getSubSequence() == fragment) {
            //Packet is last fragment, message is complete
            q->unlock();
            return true;
        }
        fragment++;
        node = node->next;
    }
    //End of queue reached
    q->unlock();
    return false;
}

bool RxModule::isNewMessageAvailable() {
    pthread_mutex_lock(&rx_mutex);
    bool result = !available_messages.empty();
    pthread_mutex_unlock(&rx_mutex);

    return result;
}

void RxModule::waitForNextAvailableMessage(pthread_mutex_t * callerMutex) {
    if (isNewMessageAvailable()) {
        return;
    }
    pthread_cond_wait(&available_msg_cond, callerMutex);
}

char * RxModule::getNextCompleteMessage(uint16_t * len, int * error, uint8_t * port) {
    *len = 0;
    *port = 0;
    *error = CODE_SUCCESS;
    if (!isActive()) {
        *error = MODULE_INACTIVE;
        LOG_WARN("RX: Module currently inactive, cannot consume messages");
        *len = 0;
        return NULL;
    }
    if (!isNewMessageAvailable()) {
        *error = CODE_NO_MESSAGES_AVAILABLE;
        LOG_WARN("RX: Trying to consume message, although no new messages are available");
        *len = 0;
        return NULL;
    }
    //TODO: locks
    std::map<uint8_t, uint8_t>::iterator it = available_messages.begin();
    bool messageAssembled = false;
    std::vector<char> assembledData;
    while (!messageAssembled) {
        S3TP_PACKET * pkt = inBuffer->getNextPacket(it->first)->pkt;
        if (pkt->hdr.seq_port != current_port_sequence[it->first]) {
            *error = CODE_ERROR_INCONSISTENT_STATE;
            LOG_ERROR("RX: inconsistency between packet sequence port and expected sequence port");
            return NULL;
        }
        char * end = pkt->pdu + (sizeof(char) * pkt->hdr.getPduLength());
        assembledData.insert(assembledData.end(), pkt->pdu, end);
        *len += pkt->hdr.getPduLength();
        current_port_sequence[it->first]++;
        if (!pkt->hdr.moreFragments()) {
            *port = it->first;
            messageAssembled = true;
        }
    }
    //Message was assembled correctly, checking if there are further available messages
    if (isCompleteMessageForPortAvailable(it->first)) {
        //New message is available, notify
        available_messages[it->first] = 1;
        pthread_cond_signal(&available_msg_cond);
    } else {
        available_messages.erase(it->first);
    }
    //Copying the entire data array, as vector memory will be released at end of function
    char * data = new char[assembledData.size()];
    memcpy(data, assembledData.data(), assembledData.size());
    return data;
}

int RxModule::comparePriority(S3TP_PACKET_WRAPPER* element1, S3TP_PACKET_WRAPPER* element2) {
    int comp = 0;
    uint8_t seq1, seq2, offset;
    pthread_mutex_lock(&rx_mutex);
    offset = to_consume_global_seq;
    //First check global seq number for comparison
    seq1 = element1->pkt->hdr.getGlobalSequence() - offset;
    seq2 = element2->pkt->hdr.getGlobalSequence() - offset;
    if (seq1 < seq2) {
        comp = -1; //Element 1 is lower, hence has higher priority
    } else if (seq1 > seq2) {
        comp = 1; //Element 2 is lower, hence has higher priority
    }
    if (comp != 0) {
        pthread_mutex_unlock(&rx_mutex);
        return comp;
    }
    offset = current_port_sequence[element1->pkt->hdr.getPort()];
    seq1 = element1->pkt->hdr.seq_port - offset;
    seq2 = element2->pkt->hdr.seq_port - offset;
    if (seq1 < seq2) {
        comp = -1; //Element 1 is lower, hence has higher priority
    } else if (seq1 > seq2) {
        comp = 1; //Element 2 is lower, hence has higher priority
    }
    pthread_mutex_unlock(&rx_mutex);
    return comp;
}