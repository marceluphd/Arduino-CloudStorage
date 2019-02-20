#pragma once

#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include "Http/Client.h"
#include "Http/EspClient.h"
#include "Utils/ResultWrapper.h"
#include <functional>

typedef std::function<void(String)> KeyChangedCallback;
enum PopFrom {
  PopFrom_Start,
  PopFrom_End
};
  
template <class RequestType>
class BaseCloudStorage {
public:
  BaseCloudStorage(String baseServerUrl, String username = "", String password = ""): 
    _username(username), _password(password), _baseServerUrl(baseServerUrl), _listenCallback([](String){}) {}
  
  void setCredentials(String name, String pass) {
    this->_username = name;
    this->_password = pass;
  }

  void startListeningForUpdates(String host, int port) {
    client.onMessage([&](websockets::WebsocketsMessage msg) {
      String data(msg.data().c_str());
      
      Serial.print("Got Data: ");
      Serial.println(data);
      
      StaticJsonBuffer<300> jsonBuffer;
      JsonObject& root = jsonBuffer.parseObject(data);

      if(root["error"]) return;
      String type = root["type"];

      if(type == "value-changed") {
        String key = root["result"]["key"];
        this->_listenCallback(key);
      }
    });
    client.connect(host.c_str(), port, "/");
    if(client.available()) {
      // send login string
      String loginJson = "{\"type\": \"login\",\"username\": \"" + this->_username + "\", \"password\": \"" + this->_password + "\" }";
      client.send(loginJson.c_str());
    }
  }

  void listen(String key) {
    String listenJson = "{\"type\": \"listen\", \"key\": \"" + key + "\" }";
    client.send(listenJson.c_str());
  }
  
  void setChangeCallback(KeyChangedCallback callback) {
    this->_listenCallback = callback;
  }

  bool isListeningForUpdates() {
    return client.available();
  }

  void loop() {
    client.poll();
  }

  // Method for storing a key/value pair
  template <class Ty>
  bool put(String key, Ty value) {
    // Build request json 
    String jsonString = buildStoreObjectRequestJson(key, value);

    // Construct http request
    RequestType request(
      _baseServerUrl + "/data/object", 
      http::Method::POST, 
      jsonString
    );
    request.addHeader("Content-Type", "application/json");
    
    // Execute request and return success status
    http::Response response = request.execute();
    if(response.statusCode != 200) return false;

    StaticJsonBuffer<300> jsonBuffer;
    JsonObject& root = jsonBuffer.parseObject(response.body);

    return !root["error"]; // return isOk
  }

  // Method for retrieving key/value pair
  template <class Ty>
  cloud_storage_utils::ResultWrapper<Ty> get(String key) {
    // Build request json 
    String jsonString = buildGetObjectRequestJson(key);
    
    // Construct http request
    RequestType request(
      _baseServerUrl + "/data/object", 
      http::Method::GET, 
      jsonString
    );
    request.addHeader("Content-Type", "application/json; charset=utf-8");
    
    //Execute request
    http::Response response = request.execute();
    if(response.statusCode != 200) return false;

    // Parse response body and extract the wanted value
    StaticJsonBuffer<300> jsonBuffer;
    JsonObject& root = jsonBuffer.parseObject(response.body);
    return cloud_storage_utils::ResultWrapper<Ty>(
      !root["error"],
      getValueByKey<Ty>(root["result"], key)
    );
  }

  //Method for pushing new values to arrays
  template <class Ty>
  bool add(String collectionKey, Ty object) {
    // Build request json 
    String jsonString = buildAddObjectRequestJson(collectionKey, object);

    // Construct http request
    RequestType request(
      _baseServerUrl + "/data/collection", 
      http::Method::POST, 
      jsonString
    );
    request.addHeader("Content-Type", "application/json; charset=utf-8");

    // Execute request and return success status
    http::Response response = request.execute();
    if(response.statusCode != 200) return false;
    
    StaticJsonBuffer<300> jsonBuffer;
    JsonObject& root = jsonBuffer.parseObject(response.body);

    return !root["error"]; // return isOk
  }

  // Method for popping values from arrays in the server
  template <class Ty>
  cloud_storage_utils::PopResultWrapper<Ty> pop(String collectionKey, PopFrom popFrom) {
    // Build request json 
    String jsonString = buildPopRequestJson(collectionKey, popFrom);

    // Construct http request
    RequestType request(
      _baseServerUrl + "/data/collection/pop", 
      http::Method::GET, 
      jsonString
    );
    request.addHeader("Content-Type", "application/json; charset=utf-8");

    // Execute request and return success status
    http::Response response = request.execute();
    if(response.statusCode != 200) return false;
    
    StaticJsonBuffer<300> jsonBuffer;
    JsonObject& root = jsonBuffer.parseObject(response.body);

    if(root["error"]) {
      return false;
    }

    return cloud_storage_utils::PopResultWrapper<Ty>(
      true,
      root["result"]["value"],
      !root["result"]["empty"]
    );
  }

  /* Atomic Operations */

  //Increment
  cloud_storage_utils::ResultWrapper<int> inc(String key, int value = 1) {
    return atomic<int>(key, "inc", value);
  }

  //Decrement
  cloud_storage_utils::ResultWrapper<int> dec(String key, int value = 1) {
    return atomic<int>(key, "dec", value);
  }

  //Replace an item if it is a new minimum
  template <class Ty>
  cloud_storage_utils::ResultWrapper<Ty> put_min(String key, Ty value) {
    return atomic<Ty>(key, "min", value);
  }
  
  //Replace an item if it is a new maximum
  template <class Ty>
  cloud_storage_utils::ResultWrapper<Ty> put_max(String key, Ty value) {
    return atomic<Ty>(key, "max", value);
  }

  //Replace an item if it is a new maximum
  cloud_storage_utils::ResultWrapper<String> datetime(String key) {
    return atomic<String>(key, "date", "");
  }

  /* Aggregate Operations */
  //Get the minimum item in array
  template <class Ty>
  cloud_storage_utils::ResultWrapper<Ty> min(String collectionKey) {
    return aggregation<Ty>(collectionKey, "min");
  }

  //Get the maximum item in array
  template <class Ty>
  cloud_storage_utils::ResultWrapper<Ty> max(String collectionKey) {
    return aggregation<Ty>(collectionKey, "max");
  }

  //Get the average of array
  cloud_storage_utils::ResultWrapper<double> avg(String collectionKey) {
    return aggregation<double>(collectionKey, "average");
  }

  //Get the size of array
  cloud_storage_utils::ResultWrapper<int> count(String collectionKey) {
    return aggregation<int>(collectionKey, "count");
  }

  //Get the sum of array
  template <class Ty>
  cloud_storage_utils::ResultWrapper<Ty> sum(String collectionKey) {
    return aggregation<Ty>(collectionKey, "count");
  }
  
private:
  String _username, _password;
  const String _baseServerUrl;
  websockets::WebsocketsClient client;
  KeyChangedCallback _listenCallback;

  // Utility method for constructing *Store* request json string
  template <class Type>
  String buildStoreObjectRequestJson(String key, Type value) {
    // Compose request json object
    StaticJsonBuffer<200> jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();
    root["username"] = _username;
    root["password"] = _password;
    root["key"] = key;
    root["value"] = value;

    // Return string form of the object
    String jsonString;
    root.printTo(jsonString);
    return jsonString;
  }

  // Utility method for constructing *Get* request json string
  String buildGetObjectRequestJson(String key) {
    // Compose request json object
    StaticJsonBuffer<200> jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();
    root["username"] = _username;
    root["password"] = _password;
    root["key"] = key;

    // Return string form of the object
    String jsonString;
    root.printTo(jsonString);
    return jsonString;
  }

  // Utility method for constructing *Add* request json string
  template <class Ty>
  String buildAddObjectRequestJson(String collectionKey, Ty object) {
    // Compose request json object
    StaticJsonBuffer<200> jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();
    root["username"] = _username;
    root["password"] = _password;
    root["collection_key"] = collectionKey;
    root["value"] = object;

    // Return string form of the object
    String jsonString;
    root.printTo(jsonString);
    return jsonString;
  }

  // Utility method for constructing *Pop* request json string
  String buildPopRequestJson(String collectionKey, PopFrom popFrom) {
    // Compose request json object
    StaticJsonBuffer<200> jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();
    root["username"] = _username;
    root["password"] = _password;
    root["collection_key"] = collectionKey;
    root["position"] = (popFrom == PopFrom_Start? "first": "last");

    // Return string form of the object
    String jsonString;
    root.printTo(jsonString);
    return jsonString;
  }

  // Utility method for constructing generic *Atomic* request json string
  template <class Ty>
  String buildAtomicRequestJson(String key, String action, Ty value) {
    // Compose request json object
    StaticJsonBuffer<200> jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();
    root["username"] = _username;
    root["password"] = _password;
    root["key"] = key;
    root["action"] = action;
    root["value"] = value;

    // Return string form of the object
    String jsonString;
    root.printTo(jsonString);
    return jsonString;
  }

  // Method for accessing nested json objects with '.' seperated keys.
  // for examples "name.first.english" for accessing name:{.., first:{ english: "MyName", .... }}
  template <class Type>
  Type getValueByKey(JsonObject& root, String key) {
    // in case no '.' is in the key, return the corresponding value
    if(key.indexOf('.') == -1) return root[key];

    // split key into parent and remainder
    // example: "user.name.fullname" => parentKey("user"), remainder("name.fullname")
    String parentKey = key.substring(0, key.indexOf('.'));
    String remainder = key.substring(key.indexOf('.') + 1);

    // traverse the parentKey object
    return getValueByKey<Type>(root[parentKey], remainder);
  }

  //Generic Atomic Request
  template <class Ty>
  cloud_storage_utils::ResultWrapper<Ty> atomic(String key, String action, Ty value) {
    // Build request json 
    String jsonString = buildAtomicRequestJson<Ty>(key, action, value);
    
    // Construct http request
    RequestType request(
      _baseServerUrl + "/data/object/atomic", 
      http::Method::GET, 
      jsonString
    );
    request.addHeader("Content-Type", "application/json; charset=utf-8");
    
    //Execute request
    http::Response response = request.execute();
    if(response.statusCode != 200) return false;

    // Parse response body and extract the wanted value
    StaticJsonBuffer<300> jsonBuffer;
    JsonObject& root = jsonBuffer.parseObject(response.body);
    return cloud_storage_utils::ResultWrapper<Ty>(
      !root["error"],
      getValueByKey<Ty>(root["result"], key)
    );
  }

  // Utility method for constructing generic *Aggregate* request json string
  String buildAggregationRequestJson(String collectionKey, String action) {
    // Compose request json object
    StaticJsonBuffer<200> jsonBuffer;
    JsonObject& root = jsonBuffer.createObject();
    root["username"] = _username;
    root["password"] = _password;
    root["collection_key"] = collectionKey;
    root["action"] = action;

    // Return string form of the object
    String jsonString;
    root.printTo(jsonString);
    return jsonString;
  }


  //Generic Aggregation Request
  template <class Ty>
  cloud_storage_utils::ResultWrapper<Ty> aggregation(String key, String action) {
    // Build request json 
    String jsonString = buildAggregationRequestJson(key, action);
    
    // Construct http request
    RequestType request(
      _baseServerUrl + "/data/collection/aggregate", 
      http::Method::GET, 
      jsonString
    );
    request.addHeader("Content-Type", "application/json; charset=utf-8");
    
    //Execute request
    http::Response response = request.execute();
    if(response.statusCode != 200) return false;

    // Parse response body and extract the wanted value
    StaticJsonBuffer<300> jsonBuffer;
    JsonObject& root = jsonBuffer.parseObject(response.body);
    return cloud_storage_utils::ResultWrapper<Ty>(
      !root["error"],
      root["result"]
    );
  }
};

typedef BaseCloudStorage<http::GenericEspRequest> CloudStorage;