#include <ipa_canopen_master/canopen.h>

using namespace ipa_canopen;

#pragma pack(push) /* push current alignment to stack */
#pragma pack(1) /* set alignment to 1 byte boundary */

struct NMTcommand{
    enum Command{
        Start = 1,
        Stop = 2,
        Prepare = 128,
        Reset = 129,
        Reset_Com = 130
    };
    uint8_t command;
    uint8_t node_id;
    
    struct Frame: public FrameOverlay<NMTcommand>{
        Frame(uint8_t node_id, const Command &c) : FrameOverlay(ipa_can::Header(0)) {
            data.command = c;
            data.node_id = node_id;
        }
    };
};

#pragma pack(pop) /* pop previous alignment from stack */

Node::Node(const boost::shared_ptr<ipa_can::Interface> interface, const boost::shared_ptr<ObjectDict> dict, uint8_t node_id, const boost::shared_ptr<SyncProvider> sync)
: node_id_(node_id), interface_(interface), sync_(sync) , state_(Unknown), sdo_(interface, dict, node_id), pdo_(interface){
    nmt_listener_ = interface_->createMsgListener( ipa_can::Header(0x700 + node_id_), ipa_can::Interface::FrameDelegate(this, &Node::handleNMT));
    
    sdo_.init();
    getStorage()->entry(heartbeat_, 0x1017);

    reset_com();
}
    
const Node::State& Node::getState(){
    boost::timed_mutex::scoped_lock lock(mutex); // TODO: timed lock?
    return state_;
}

void Node::reset_com(){
    boost::timed_mutex::scoped_lock lock(mutex); // TODO: timed lock?
    getStorage()->clear();
    interface_->send(NMTcommand::Frame(node_id_, NMTcommand::Reset_Com));
    wait_for(BootUp, boost::posix_time::seconds(1));
    state_ = PreOperational;
    heartbeat_.set(heartbeat_.desc().value().get<uint16_t>());

}
void Node::reset(){
    boost::timed_mutex::scoped_lock lock(mutex); // TODO: timed lock?
    getStorage()->clear();
    interface_->send(NMTcommand::Frame(node_id_, NMTcommand::Reset));
    wait_for(BootUp, boost::posix_time::seconds(1));
    state_ = PreOperational;
    heartbeat_.set(heartbeat_.desc().value().get<uint16_t>());
}

void Node::prepare(){
    boost::timed_mutex::scoped_lock lock(mutex); // TODO: timed lock?
    if(state_ == BootUp){
        // ERROR
    }
    interface_->send(NMTcommand::Frame(node_id_, NMTcommand::Prepare));
    wait_for(PreOperational, boost::posix_time::milliseconds(heartbeat_.get_cached() * 3));
}
void Node::start(){
    boost::timed_mutex::scoped_lock lock(mutex); // TODO: timed lock?
    if(state_ == BootUp){
        // ERROR
    }
    else if(state_ == PreOperational){
        pdo_.init(getStorage());
        getStorage()->init_all();
    }
    interface_->send(NMTcommand::Frame(node_id_, NMTcommand::Start));
    wait_for(Operational, boost::posix_time::milliseconds(heartbeat_.get_cached() * 3));
}
void Node::stop(){
    boost::timed_mutex::scoped_lock lock(mutex); // TODO: timed lock?
    if(state_ == BootUp){
        // ERROR
    }
    interface_->send(NMTcommand::Frame(node_id_, NMTcommand::Stop));
    wait_for(Stopped, boost::posix_time::milliseconds(heartbeat_.get_cached() * 3));
}

void Node::switchState(const uint8_t &s){
    switch(s){
        case Operational:
            if(!sync_listener_ && sync_)
                sync_listener_ = sync_->add(SyncProvider::SyncDelegate(&pdo_, &PDOMapper::sync));
            break;
        case BootUp:
        case PreOperational:
        case Stopped:
            sync_listener_.reset();
            break;
        default:
            //error
            ;
    }
    state_ = (State) s;
}
void Node::handleNMT(const ipa_can::Frame & msg){
    boost::mutex::scoped_lock cond_lock(cond_mutex);
    assert(msg.dlc == 1);
    switchState(msg.data[0]);
    cond_lock.unlock();
    cond.notify_one();
    
}

template<typename T> void Node::wait_for(const State &s, const T &timeout){
    boost::mutex::scoped_lock cond_lock(cond_mutex);
    boost::system_time abs_time = boost::get_system_time() + timeout;
    
    if(timeout == boost::posix_time::milliseconds(0)){
        boost::this_thread::sleep(boost::posix_time::milliseconds(100));
        switchState(s);
        boost::this_thread::yield();
    }
    
    while(s != state_)
    {
        if(!cond.timed_wait(cond_lock,abs_time))
        {
            if(s != state_){
                throw TimeoutException();
            }
        }
   }
}

