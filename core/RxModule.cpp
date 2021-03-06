//
// Created by Lorenzo Donini on 02/09/16.
//

#include "RxModule.h"

RxModule::RxModule() {
    to_consume_global_seq = 0;
    receiving_window = 0;
    lastReceivedGlobalSeq = to_consume_global_seq;
    pthread_mutex_init(&rx_mutex, NULL);
    pthread_cond_init(&available_msg_cond, NULL);
    inBuffer = new Buffer(this);
    statusInterface = NULL;
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

void RxModule::reset() {
    pthread_mutex_lock(&rx_mutex);
    receiving_window = 0;
    to_consume_global_seq = 0;
    lastReceivedGlobalSeq = to_consume_global_seq;
    inBuffer->clear();
    current_port_sequence.clear();
    available_messages.clear();
    open_ports.clear();
    pthread_mutex_unlock(&rx_mutex);
}

void RxModule::setStatusInterface(StatusInterface * statusInterface) {
    pthread_mutex_lock(&rx_mutex);
    this->statusInterface = statusInterface;
    pthread_mutex_unlock(&rx_mutex);
}

void RxModule::startModule() {
    pthread_mutex_lock(&rx_mutex);
    receiving_window = 0;
    active = true;
    pthread_mutex_unlock(&rx_mutex);
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
    if (length > MAX_LEN_S3TP_PACKET) {
        //TODO: handle error
    }
    //Copying packet. Data argument is not needed anymore afterwards
    S3TP_PACKET * packet = new S3TP_PACKET((char *)data, length, (uint8_t)channel);
    int result = handleReceivedPacket(packet);
    //TODO: handle error
}

void RxModule::handleLinkStatus(bool linkStatus) {
    pthread_mutex_lock(&rx_mutex);
    LOG_DEBUG("Link status changed");
    if (statusInterface != NULL) {
        statusInterface->onLinkStatusChanged(linkStatus);
    }
    pthread_mutex_unlock(&rx_mutex);
}

void RxModule::handleBufferEmpty(int channel) {
    //The channel queue is not full anymore, so we can start writing on it again
    pthread_mutex_lock(&rx_mutex);
    if (statusInterface != NULL) {
        statusInterface->onChannelStatusChanged((uint8_t)channel, true);
    }
    pthread_mutex_unlock(&rx_mutex);
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
    open_ports[port] = 1;
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
        pthread_mutex_unlock(&rx_mutex);
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

int RxModule::handleReceivedPacket(S3TP_PACKET * packet) {
    if (!isActive()) {
        return MODULE_INACTIVE;
    }

    //Checking CRC
    S3TP_HEADER * hdr = packet->getHeader();
    //TODO: check if pdu length + header size > total length (there might've been an error), otherwise buffer overflow
    if (!verify_checksum(packet->getPayload(), hdr->getPduLength(), hdr->crc)) {
        LOG_WARN(std::string("Wrong CRC for packet " + std::to_string((int)hdr->getGlobalSequence())));
        return CODE_ERROR_CRC_INVALID;
    }

    S3TP_MSG_TYPE type = hdr->getMessageType();
    if (type == S3TP_MSG_SYNC) {
        S3TP_SYNC * sync = (S3TP_SYNC*)packet->getPayload();
        LOG_DEBUG("RX: Sync Packet received");
        synchronizeStatus(*sync);
        return CODE_SUCCESS;
    } else if (type != S3TP_MSG_DATA) {
        //Not recognized data message
        LOG_WARN(std::string("Unrecognized message type received: " + std::to_string((int)type)));
        return CODE_ERROR_INVALID_TYPE;
    }

    if (!isPortOpen(hdr->getPort())) {
        //Dropping packet right away
        LOG_INFO(std::string("Incoming packet " + std::to_string(hdr->getGlobalSequence())
                             + "for port " + std::to_string(hdr->getPort())
                             + " was dropped because port is closed"));
        return CODE_ERROR_PORT_CLOSED;
    }

    int result = inBuffer->write(packet);
    if (result != CODE_SUCCESS) {
        //Something bad happened, couldn't put packet in buffer
        return result;
    }

    LOG_DEBUG(std::string("RX: Packet received from SPI to port "
                          + std::to_string((int)hdr->getPort())
                          + " -> glob_seq " + std::to_string((int)hdr->getGlobalSequence())
                          + ", sub_seq " + std::to_string((int)hdr->getSubSequence())
                          + ", port_seq " + std::to_string((int)hdr->seq_port)));

    pthread_mutex_lock(&rx_mutex);
    if (isCompleteMessageForPortAvailable(hdr->getPort())) {
        //New message is available, notify
        available_messages[hdr->getPort()] = 1;
        pthread_cond_signal(&available_msg_cond);
    }
    pthread_mutex_unlock(&rx_mutex);

    //This variable doesn't need locking, as it is a purely internal counter
    uint8_t relativeGlobSeq = (hdr->getGlobalSequence() - to_consume_global_seq);
    if (relativeGlobSeq < RECEIVING_WINDOW_SIZE && relativeGlobSeq > lastReceivedGlobalSeq) {
        lastReceivedGlobalSeq = hdr->getGlobalSequence();
    }
    receiving_window++;
    if (receiving_window >= RECEIVING_WINDOW_SIZE) {
        //Update global sequence number and flush queues
        LOG_DEBUG("Receiving window reached. Flushing queues now..");
        flushQueues();
        receiving_window = 0;
    }

    return CODE_SUCCESS;
}

void RxModule::synchronizeStatus(S3TP_SYNC& sync) {
    pthread_mutex_lock(&rx_mutex);
    for (int i=0; i<DEFAULT_MAX_OUT_PORTS; i++) {
        if (sync.port_seq[i] != 0) {
            current_port_sequence[i] = sync.port_seq[i];
        }
    }
    //TODO: clear useless stuff
    //Check all queues and remove useless stuff
    //uint8_t seq1 = sync.tx_global_seq - to_consume_global_seq;
    //uint8_t seq2 = lastReceivedGlobalSeq - to_consume_global_seq;
    lastReceivedGlobalSeq = sync.tx_global_seq;

    //Notify main module
    LOG_DEBUG("Receiver sequences synchronized correctly");
    statusInterface->onSynchronization(sync.syncId);
    pthread_mutex_unlock(&rx_mutex);
}

bool RxModule::isCompleteMessageForPortAvailable(int port) {
    //Method is called internally, no need for locks
    PriorityQueue<S3TP_PACKET*> * q = inBuffer->getQueue(port);
    q->lock();
    PriorityQueue_node<S3TP_PACKET*> * node = q->getHead();
    uint8_t fragment = 0;
    while (node != NULL) {
        S3TP_PACKET * pkt = node->element;
        S3TP_HEADER * hdr = pkt->getHeader();
        if (hdr->seq_port != (current_port_sequence[port] + fragment)) {
            //Packet in queue is not the one with highest priority
            break; //Will return false
        } else if (hdr->moreFragments() && hdr->getSubSequence() != fragment) {
            //Current fragment number is not the expected number,
            // i.e. at least one fragment is missing to complete the message
            break; //Will return false
        } else if (!hdr->moreFragments() && hdr->getSubSequence() == fragment) {
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

    pthread_mutex_lock(&rx_mutex);
    std::map<uint8_t, uint8_t>::iterator it = available_messages.begin();
    bool messageAssembled = false;
    std::vector<char> assembledData;
    while (!messageAssembled) {
        S3TP_PACKET * pkt = inBuffer->getNextPacket(it->first);
        S3TP_HEADER * hdr = pkt->getHeader();
        if (hdr->seq_port != current_port_sequence[it->first]) {
            *error = CODE_ERROR_INCONSISTENT_STATE;
            LOG_ERROR("RX: inconsistency between packet sequence port and expected sequence port");
            return NULL;
        }
        char * end = pkt->getPayload() + (sizeof(char) * hdr->getPduLength());
        assembledData.insert(assembledData.end(), pkt->getPayload(), end);
        *len += hdr->getPduLength();
        current_port_sequence[it->first]++;
        if (!hdr->moreFragments()) {
            *port = it->first;
            messageAssembled = true;
            // Not updating the global sequence right away,
            // as this will be done after the recv window has been filled
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
    //Increase global sequence
    pthread_mutex_unlock(&rx_mutex);
    //Copying the entire data array, as vector memory will be released at end of function
    char * data = new char[assembledData.size()];
    memcpy(data, assembledData.data(), assembledData.size());
    return data;
}

void RxModule::flushQueues() {
    std::set<int> activeQueues = inBuffer->getActiveQueues();
    uint8_t pktGlobalSequence;
    //Will flush only queues which currently hold data
    for (auto port : activeQueues) {
        PriorityQueue<S3TP_PACKET*>* queue = inBuffer->getQueue(port);
        if (queue->isEmpty()) {
            LOG_DEBUG("WTF????");
            //TODO: there's some serious error here
            continue;
        }
        S3TP_PACKET * packet = queue->peek();
        pktGlobalSequence = packet->getHeader()->getGlobalSequence() - to_consume_global_seq;
        if (pktGlobalSequence >= MAX_REORDERING_WINDOW) {
            // pktGlobalSequence < glob_seq
            // Packet is too old, clearing the queue
            inBuffer->clearQueueForPort((uint8_t) port);
            //TODO: send error to application
            continue;
        }
    }

    //Updating current global sequence
    /*if (highestReceivedGlobalSeq - MAX_REORDERING_WINDOW != to_consume_global_seq) {
        LOG_INFO("Packets were lost somewhere...");
    }*/
    to_consume_global_seq = lastReceivedGlobalSeq;
}

int RxModule::comparePriority(S3TP_PACKET* element1, S3TP_PACKET* element2) {
    int comp = 0;
    uint8_t seq1, seq2, offset;
    pthread_mutex_lock(&rx_mutex);

    offset = current_port_sequence[element1->getHeader()->getPort()];
    seq1 = element1->getHeader()->seq_port - offset;
    seq2 = element2->getHeader()->seq_port - offset;
    if (seq1 < seq2) {
        comp = -1; //Element 1 is lower, hence has higher priority
    } else if (seq1 > seq2) {
        comp = 1; //Element 2 is lower, hence has higher priority
    }
    pthread_mutex_unlock(&rx_mutex);
    return comp;
}

bool RxModule::isElementValid(S3TP_PACKET * element) {
    //Not needed for Rx Module. Return true by default
    return true;
}

bool RxModule::maximumWindowExceeded(S3TP_PACKET* queueHead, S3TP_PACKET* newElement) {
    uint8_t offset, headSeq, newSeq;

    pthread_mutex_lock(&rx_mutex);
    offset = to_consume_global_seq;
    headSeq = queueHead->getHeader()->getGlobalSequence() - offset;
    newSeq = newElement->getHeader()->getGlobalSequence() - offset;
    pthread_mutex_unlock(&rx_mutex);

    return abs(newSeq - headSeq) > MAX_REORDERING_WINDOW;
}