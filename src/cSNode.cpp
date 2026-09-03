#include <node.h>
#include "cSNode.h"

namespace cS {

// this should be Node
void NodeData::asyncCallback(Async *async)
{
    NodeData *nodeData = (NodeData *) async->getData();

    nodeData->asyncMutex->lock();
    for (Poll *p : nodeData->transferQueue) {
        Socket *s = (Socket *) p;
        TransferData *transferData = (TransferData *) s->getUserData();

        s->reInit(nodeData->loop, transferData->fd);
        s->setCb(transferData->pollCb);
        s->start(nodeData->loop, s, s->setPoll(transferData->pollEvents));

        s->nodeData = transferData->destination;
        s->setUserData(transferData->userData);
        auto *transferCb = transferData->transferCb;

        delete transferData;
        transferCb(s);
    }

    for (Poll *p : nodeData->changePollQueue) {
        Socket *s = (Socket *) p;
        s->change(s->nodeData->loop, s, s->getPoll());
    }

    nodeData->changePollQueue.clear();
    nodeData->transferQueue.clear();
    nodeData->asyncMutex->unlock();
}

Node::Node(int recvLength, int prePadding, int postPadding, bool useDefaultLoop) {
    nodeData = new NodeData;
    nodeData->recvBufferMemoryBlock = new char[recvLength];
    nodeData->recvBuffer = nodeData->recvBufferMemoryBlock + prePadding;
    nodeData->recvLength = recvLength - prePadding - postPadding;

    nodeData->tid = pthread_self();
    loop = Loop::createLoop(useDefaultLoop);

    // each node has a context
    nodeData->netContext = new Context();

    nodeData->loop = loop;
    nodeData->asyncMutex = &asyncMutex;
    nodeData->corkState = new NodeData::CorkState;

    nodeData->pool = new NodeData::BlockPool;

    nodeData->clientContext = SSL_CTX_new(TLS_method());
    SSL_CTX_set_min_proto_version(nodeData->clientContext, TLS1_VERSION);
    SSL_CTX_set_max_proto_version(nodeData->clientContext, TLS1_2_VERSION);
}

void Node::run() {
    nodeData->tid = pthread_self();
    loop->run();
}

void Node::poll() {
    loop->poll();
}

Node::~Node() {
    delete [] nodeData->recvBufferMemoryBlock;
    SSL_CTX_free(nodeData->clientContext);

    for (std::vector<char *> &blocks : nodeData->pool->free) {
        for (char *block : blocks) {
            delete [] block;
        }
    }
    delete nodeData->pool;
    delete nodeData->corkState;
    delete nodeData->netContext;
    delete nodeData;
    loop->destroy();
}

}