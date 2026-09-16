#include <string>
#include "cWS.h"
#include "Addon.h"

void Initialize(Local<Object> exports) {
  Isolate *isolate = Isolate::GetCurrent();
  exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "server").ToLocalChecked(),
              Namespace<cWS::SERVER>(isolate).object);
  exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "client").ToLocalChecked(),
              Namespace<cWS::CLIENT>(isolate).object);

  NODE_SET_METHOD(exports, "getSSLContext", getSSLContext);
  NODE_SET_METHOD(exports, "getBufferedAmount", getBufferedAmount<cWS::SERVER>);
  NODE_SET_METHOD(exports, "setUserData", setUserData<cWS::SERVER>);
  NODE_SET_METHOD(exports, "getUserData", getUserData<cWS::SERVER>);
  NODE_SET_METHOD(exports, "clearUserData", clearUserData<cWS::SERVER>);
  NODE_SET_METHOD(exports, "getAddress", getAddress<cWS::SERVER>);
  
  NODE_SET_METHOD(exports, "transfer", transfer);
  NODE_SET_METHOD(exports, "upgrade", upgrade);
  NODE_SET_METHOD(exports, "connect", connect);
  NODE_SET_METHOD(exports, "setNoop", setNoop);
  exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "zlibBackend").ToLocalChecked(),
               String::NewFromUtf8(isolate, cWS::zlib::backend()).ToLocalChecked()).Check();
  // The exact Node this binding was compiled against. NODE_MODULE_VERSION (the ABI in the
  // filename) is NOT enough: cWS compiles against Node's PRIVATE headers, and their struct
  // layouts change between minor releases of the same major while the ABI number stays put
  // (Node 24.7 -> 24.13 changed an Environment member from v8::LocalVector to std::deque and
  // added two more, shifting every offset after it). Loading such a binding does not fail; it
  // silently reads the wrong memory and sockets misbehave. shared.ts compares this against
  // process.versions.node and warns.
  exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "builtForNode").ToLocalChecked(),
               String::NewFromUtf8(isolate, NODE_VERSION).ToLocalChecked()).Check();
  hub.getNodeData()->corkState->enabled = corkEnabledFromEnv();
  registerCheck(isolate);
  exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "sendThread").ToLocalChecked(),
               String::NewFromUtf8(isolate, cS::SendWorker::status()).ToLocalChecked()).Check();
  // The receive worker starts lazily (first server that asks for it), so this is a getter.
  exports->SetNativeDataProperty(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "recvThread").ToLocalChecked(),
               [](Local<Name>, const PropertyCallbackInfo<Value> &info) {
                 info.GetReturnValue().Set(String::NewFromUtf8(info.GetIsolate(), cS::RecvWorker::status()).ToLocalChecked());
               }).Check();
}

NODE_MODULE(addon, Initialize)