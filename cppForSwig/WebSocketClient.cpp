////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2018, goatpig.                                              //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //                                      
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "WebSocketClient.h"

TransactionalMap<struct lws*, shared_ptr<WebSocketClient>> 
   WebSocketClient::objectMap_;

////////////////////////////////////////////////////////////////////////////////
static struct lws_protocols protocols[] = {
   /* first protocol must always be HTTP handler */

   {
      "armory-bdm-protocol",
      WebSocketClient::callback,
      sizeof(struct per_session_data__client),
      per_session_data__client::rcv_size,
   },

{ NULL, NULL, 0, 0 } /* terminator */
};

////////////////////////////////////////////////////////////////////////////////
void WebSocketClient::pushPayload(Socket_WritePayload& write_payload,
   shared_ptr<Socket_ReadPayload> read_payload)
{
   unsigned id;
   do
   {
      id = rand();
   } while (id == UINT32_MAX || id == WEBSOCKET_CALLBACK_ID);

   if (read_payload != nullptr)
   {
      //create response object
      auto response = make_shared<WriteAndReadPacket>(id, read_payload);

      //set response id
      readPackets_.insert(make_pair(response->id_, move(response)));   
   }
   else
   {
   }
   
   auto&& data_vector = 
      WebSocketMessage::serialize(id, write_payload.data_);

   //push packets to write queue
   for(auto& data : data_vector)
      writeQueue_.push_back(move(data));

   //trigger write callback
   auto wsiptr = (struct lws*)wsiPtr_.load(memory_order_relaxed);
   lws_callback_on_writable(wsiptr);
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<WebSocketClient> WebSocketClient::getNew(
   const string& addr, const string& port)
{
   WebSocketClient* objPtr = new WebSocketClient(addr, port);
   shared_ptr<WebSocketClient> newObject;
   newObject.reset(objPtr);
   
   auto wsiptr = (struct lws*)newObject->wsiPtr_.load(memory_order_relaxed);

   objectMap_.insert(move(make_pair(wsiptr, newObject)));
   return newObject;
}

////////////////////////////////////////////////////////////////////////////////
void WebSocketClient::setIsReady(bool status)
{
   if (ctorProm_ != nullptr)
   {
      try
      {
         ctorProm_->set_value(status);
      }
      catch(future_error&)
      { }
   }
}

////////////////////////////////////////////////////////////////////////////////
void WebSocketClient::init()
{
   Arguments::serializeID(false);
   run_.store(1, memory_order_relaxed);

   //setup context
   struct lws_context_creation_info info;
   memset(&info, 0, sizeof info);
   
   info.port = CONTEXT_PORT_NO_LISTEN;
   info.protocols = protocols;
   info.ws_ping_pong_interval = 0;
   info.gid = -1;
   info.uid = -1;

   auto contextptr = lws_create_context(&info);
   if (contextptr == NULL) 
      throw LWS_Error("failed to create LWS context");

   contextPtr_.store(contextptr, memory_order_relaxed);

   //connect to server
   struct lws_client_connect_info i;
   memset(&i, 0, sizeof(i));
   
   //i.address = ip.c_str();
   i.port = WEBSOCKET_PORT;
   const char *prot, *p;
   char path[300];
   lws_parse_uri((char*)addr_.c_str(), &prot, &i.address, &i.port, &p);

   path[0] = '/';
   lws_strncpy(path + 1, p, sizeof(path) - 1);
   i.path = path;
   i.host = i.address;
   i.origin = i.address;
   i.ietf_version_or_minus_one = -1;

   i.context = contextptr;
   i.method = nullptr;
   i.protocol = protocols[PROTOCOL_ARMORY_CLIENT].name;   

   struct lws* wsiptr;
   i.pwsi = &wsiptr;
   wsiptr = lws_client_connect_via_info(&i);   
   wsiPtr_.store(wsiptr, memory_order_relaxed);
}

////////////////////////////////////////////////////////////////////////////////
bool WebSocketClient::connectToRemote()
{
   ctorProm_ = make_unique<promise<bool>>();
   auto fut = ctorProm_->get_future();

   //start service threads
   auto readLBD = [this](void)->void
   {
      this->readService();
   };

   readThr_ = thread(readLBD);

   auto serviceLBD = [this](void)->void
   {
      service();
   };

   serviceThr_ = thread(serviceLBD);

   bool result = true;
   try
   {
      result = fut.get();
   }
   catch (future_error&)
   {
      result = false;
   }
   
   if (result == false)
   {
      LOGERR << "failed to connect to lws server";
   }

   return result;
}

////////////////////////////////////////////////////////////////////////////////
void WebSocketClient::service()
{
   int n = 0;
   auto contextptr = 
      (struct lws_context*)contextPtr_.load(memory_order_relaxed);
   contextPtr_.store(nullptr, memory_order_relaxed);

   while(run_.load(memory_order_relaxed) != 0 && n >= 0)
   {
      n = lws_service(contextptr, 50);
   }

   lws_context_destroy(contextptr);
}

////////////////////////////////////////////////////////////////////////////////
void WebSocketClient::shutdown()
{
   readPackets_.clear();

   run_.store(0, memory_order_relaxed);
   if(serviceThr_.joinable())
      serviceThr_.join();

   auto contextptr =
      (struct lws_context*)contextPtr_.load(memory_order_relaxed);
   if (contextptr != nullptr)
   {
      lws_context_destroy(contextptr);
      contextPtr_.store(nullptr, memory_order_relaxed);
   }

   readQueue_.terminate();
   if(readThr_.joinable())
      readThr_.join();
}

////////////////////////////////////////////////////////////////////////////////
int WebSocketClient::callback(struct lws *wsi, 
   enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
   struct per_session_data__client *session_data =
      (struct per_session_data__client *)user;

   switch (reason)
   {

   case LWS_CALLBACK_CLIENT_ESTABLISHED:
   {
      //ws connection established with server
      //printf("connection established with server");
      auto instance = WebSocketClient::getInstance(wsi);
      instance->setIsReady(true);
      break;
   }

   case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
   {
      LOGERR << "lws client connection error";
      if (len > 0)
      {
         auto errstr = (char*)in;
         LOGERR << "   error message: " << errstr;
      }
      else
      {
         LOGERR << "no error message was provided by lws";
      }
   }

   case LWS_CALLBACK_CLIENT_CLOSED:
   case LWS_CALLBACK_CLOSED:
   {
      WebSocketClient::destroyInstance(wsi);
      break;
   }

   case LWS_CALLBACK_CLIENT_RECEIVE:
   {
      BinaryData bdData;
      bdData.resize(len);
      memcpy(bdData.getPtr(), in, len);

      auto instance = WebSocketClient::getInstance(wsi);
      instance->readQueue_.push_back(move(bdData));
      break;
   }

   case LWS_CALLBACK_CLIENT_WRITEABLE:
   {
      auto instance = WebSocketClient::getInstance(wsi);

      BinaryData packet;
      try
      {
         packet = move(instance->writeQueue_.pop_front());
      }
      catch (IsEmpty&)
      {
         break;
      }

      auto body = packet.getPtr() + LWS_PRE;
      auto m = lws_write(wsi, 
         body, packet.getSize() - LWS_PRE,
         LWS_WRITE_BINARY);

      if (m != packet.getSize() - LWS_PRE)
      {
         LOGERR << "failed to send packet of size";
         LOGERR << "packet is " << packet.getSize() <<
            " bytes, sent " << m << " bytes";
      }

      /***
      In case several threads are trying to write to the same socket, it's
      possible their calls to callback_on_writeable may overlap, resulting 
      in a single write entry being consumed.

      To avoid this, we trigger the callback from within itself, which will 
      break out if there are no more items in the writeable stack.
      ***/
      lws_callback_on_writable(wsi);

      break;
   }

   default:
      break;
   }

   return 0;
}

////////////////////////////////////////////////////////////////////////////////
void WebSocketClient::readService()
{
   while (1)
   {
      BinaryData payload;
      try
      {
         payload = move(readQueue_.pop_front());
      }
      catch (StopBlockingLoop&)
      {
         break;
      }

      //deser packet
      auto msgid = WebSocketMessage::getMessageId(payload);

      //figure out request id, fulfill promise
      auto readMap = readPackets_.get();
      auto iter = readMap->find(msgid);
      if (iter != readMap->end())
      {
         try
         {
            iter->second->response_.processPacket(payload);
         }
         catch (exception&)
         {
            LOGWARN << "invalid packet, dropping message";
            readPackets_.erase(msgid);
         }
         
         vector<uint8_t> message;
         if (!iter->second->response_.reconstruct(message))
            continue;

         BinaryDataRef bdr(&message[0], message.size());
         BinaryData bd_hexit;
         bd_hexit.createFromHex(bdr);
         
         iter->second->payload_->callbackReturn_->callback(
            bd_hexit.getRef(), nullptr);
         readPackets_.erase(msgid);
      }
      else if (msgid == WEBSOCKET_CALLBACK_ID)
      {
         //or is it a callback command? process it locally
         WebSocketMessage response;
         response.processPacket(payload);

         vector<uint8_t> message;
         if (!response.reconstruct(message))
            continue; //callbacks should always be a single packet

         if (callbackPtr_ != nullptr)
         {
            BinaryDataRef bdr(&message[0], message.size());
            BinaryData bd_hexit;
            bd_hexit.createFromHex(bdr);
            auto&& bdr_hexit = bd_hexit.getRef();

            callbackPtr_->processArguments(bdr_hexit);
         }
      }
      else
      {
         LOGWARN << "invalid msg id";
      }
   }
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<WebSocketClient> WebSocketClient::getInstance(struct lws* ptr)
{
   auto clientMap = objectMap_.get();
   auto iter = clientMap->find(ptr);
 
   if (iter == clientMap->end())
      throw LWS_Error("no client object for this lws instance");

   return iter->second;
}

////////////////////////////////////////////////////////////////////////////////
void WebSocketClient::destroyInstance(struct lws* ptr)
{
   auto instance = getInstance(ptr);
   instance->setIsReady(false);
   instance->readPackets_.clear();
   instance->run_.store(0, memory_order_relaxed);
   objectMap_.erase(ptr);
}

////////////////////////////////////////////////////////////////////////////////
void WebSocketClient::setCallback(RemoteCallback* ptr)
{
   callbackPtr_ = ptr;
}

