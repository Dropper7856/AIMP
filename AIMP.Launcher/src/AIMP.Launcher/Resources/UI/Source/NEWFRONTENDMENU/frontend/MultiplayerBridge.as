package frontend
{
   import flash.events.Event;
   import flash.events.IOErrorEvent;
   import flash.events.ProgressEvent;
   import flash.events.SecurityErrorEvent;
   import flash.net.Socket;
   
   public class MultiplayerBridge
   {
      
      private static const HOST:String = "127.0.0.1";
      
      private static const PORT:int = 12345;
      
      private static const PROTOCOL_VERSION:String = "1";
       
      
      private var _socket:Socket;
      
      private var _buffer:String;
      
      private var _queue:Array;
      
      private var _connected:Boolean;
      
      public var onStatus:Function;
      
      public var onServerList:Function;
      
      public var onLobbyState:Function;
      
      public var onGameStart:Function;
      
      public var onConfig:Function;
      
      public var onError:Function;
      
      public var onLobbyReturn:Function;
      
      public function MultiplayerBridge()
      {
         super();
         this._buffer = "";
         this._queue = [];
         this._connected = false;
         this.connect();
      }
      
      private static function encodeValue(value:String) : String
      {
         if(value == null)
         {
            return "";
         }
         value = value.split("%").join("%25");
         value = value.split("|").join("%7C");
         value = value.split("=").join("%3D");
         value = value.split("\n").join("%0A");
         return value.split("\r").join("%0D");
      }
      
      private static function decodeValue(value:String) : String
      {
         if(value == null)
         {
            return "";
         }
         value = value.split("%0D").join("\r");
         value = value.split("%0A").join("\n");
         value = value.split("%3D").join("=");
         value = value.split("%7C").join("|");
         return value.split("%25").join("%");
      }
      
      private static function trim(value:String) : String
      {
         if(value == null)
         {
            return "";
         }
         while(value.length > 0 && (value.charAt(0) == " " || value.charAt(0) == "\r" || value.charAt(0) == "\n" || value.charAt(0) == "\t"))
         {
            value = value.substr(1);
         }
         while(value.length > 0 && (value.charAt(value.length - 1) == " " || value.charAt(value.length - 1) == "\r" || value.charAt(value.length - 1) == "\n" || value.charAt(value.length - 1) == "\t"))
         {
            value = value.substr(0,value.length - 1);
         }
         return value;
      }
      
      public function connect() : void
      {
         if(this._socket != null && this._socket.connected)
         {
            return;
         }
         this._socket = new Socket();
         this._socket.addEventListener(Event.CONNECT,this.connectedHandler,false,0,true);
         this._socket.addEventListener(Event.CLOSE,this.closeHandler,false,0,true);
         this._socket.addEventListener(ProgressEvent.SOCKET_DATA,this.socketDataHandler,false,0,true);
         this._socket.addEventListener(IOErrorEvent.IO_ERROR,this.ioErrorHandler,false,0,true);
         this._socket.addEventListener(SecurityErrorEvent.SECURITY_ERROR,this.securityErrorHandler,false,0,true);
         this._socket.connect(HOST,PORT);
      }
      
      public function createLobby(mode:String, maxPlayers:int, visibility:String, mapId:String, mapName:String) : void
      {
         var values:Object = {};
         values["mode"] = mode;
         values["maxPlayers"] = String(maxPlayers);
         values["visibility"] = visibility;
         values["mapId"] = mapId;
         values["mapName"] = mapName;
         this.send("CREATE_LOBBY",values);
      }
      
      public function refreshServers(filter:String) : void
      {
         var values:Object = {};
         values["filter"] = filter;
         this.send("REFRESH_SERVERS",values);
      }
      
      public function joinServer(serverId:String) : void
      {
         var values:Object = {};
         values["serverId"] = serverId;
         this.send("JOIN_SERVER",values);
      }
      
      public function joinDirectIp() : void
      {
         this.send("JOIN_DIRECT_IP",{});
      }
      
      public function setReady(ready:Boolean) : void
      {
         var values:Object = {};
         values["ready"] = ready ? "1" : "0";
         this.send("READY",values);
      }
      
      public function kickPlayer(slotOrClientId:String) : void
      {
         var values:Object = {};
         values["target"] = slotOrClientId;
         this.send("KICK",values);
      }
      
      public function startGame(mode:String, mapId:String, mapName:String) : void
      {
         var values:Object = {};
         values["mode"] = mode;
         values["mapId"] = mapId;
         values["mapName"] = mapName;
         this.send("START_GAME",values);
      }
      
      public function ackStartGame(sessionId:String, status:String) : void
      {
         var values:Object = {};
         values["sessionId"] = sessionId;
         values["status"] = status;
         this.send("START_GAME_ACK",values);
      }
      
      public function selectCharacter(sessionId:String, characterId:String, characterName:String, modelHint:String, source:String, confirmed:Boolean) : void
      {
         var values:Object = {};
         values["sessionId"] = sessionId;
         values["characterId"] = characterId;
         values["characterName"] = characterName;
         values["modelHint"] = modelHint;
         values["source"] = source;
         values["confirmed"] = confirmed ? "1" : "0";
         this.send("CHARACTER_SELECTED",values);
      }
      
      public function leaveLobby() : void
      {
         this.send("LEAVE",{});
      }
      
      private function connectedHandler(event:Event) : void
      {
         this._connected = true;
         this.invokeStatus("Bridge connected");
         this.send("HELLO",{
            "client":"gfx",
            "version":PROTOCOL_VERSION
         });
         this.flushQueue();
      }
      
      private function closeHandler(event:Event) : void
      {
         this._connected = false;
         this.invokeStatus("Bridge disconnected");
      }
      
      private function ioErrorHandler(event:IOErrorEvent) : void
      {
         this._connected = false;
         this.invokeError("Bridge IO error: " + event.text);
      }
      
      private function securityErrorHandler(event:SecurityErrorEvent) : void
      {
         this._connected = false;
         this.invokeError("Bridge security error: " + event.text);
      }
      
      private function socketDataHandler(event:ProgressEvent) : void
      {
         var text:String = this._socket.readUTFBytes(this._socket.bytesAvailable);
         this._buffer += text;
         var index:int = this._buffer.indexOf("\n");
         while(index >= 0)
         {
            var line:String = this._buffer.substr(0,index);
            this._buffer = this._buffer.substr(index + 1);
            this.handleLine(line);
            index = this._buffer.indexOf("\n");
         }
      }
      
      private function handleLine(line:String) : void
      {
         line = trim(line);
         if(line.length == 0)
         {
            return;
         }
         var parts:Array = line.split("|");
         var type:String = String(parts[0]);
         var values:Object = {};
         var i:int = 1;
         while(i < parts.length)
         {
            var part:String = String(parts[i]);
            var eq:int = part.indexOf("=");
            if(eq > 0)
            {
               values[part.substr(0,eq)] = decodeValue(part.substr(eq + 1));
            }
            i++;
         }
         if(type == "STATUS")
         {
            this.invokeStatus(String(values["message"]));
         }
         else if(type == "ERROR")
         {
            this.invokeError(String(values["message"]));
         }
         else if(type == "SERVER_LIST")
         {
            this.handleServerList(values);
         }
         else if(type == "LOBBY_STATE")
         {
            if(this.onLobbyState != null)
            {
               this.onLobbyState(String(values["role"]),String(values["status"]),String(values["p1"]),String(values["p2"]),String(values["p3"]),String(values["p4"]));
            }
         }
         else if(type == "START_GAME")
         {
            if(this.onGameStart != null)
            {
               this.onGameStart(String(values["mode"]),String(values["mapId"]),String(values["mapName"]),String(values["sessionId"]));
            }
         }
         else if(type == "CONFIG")
         {
            if(this.onConfig != null)
            {
               this.onConfig(values);
            }
         }
         else if(type == "LOBBY_RETURN")
         {
            if(this.onLobbyReturn != null)
            {
               this.onLobbyReturn(String(values["role"]));
            }
         }
      }
      
      private function handleServerList(values:Object) : void
      {
         var entries:Array = [];
         var count:int = int(values["count"]);
         var i:int = 0;
         while(i < count)
         {
            var entry:Object = {};
            entry.id = String(values["s" + i + "_id"]);
            entry.name = String(values["s" + i + "_name"]);
            entry.mode = String(values["s" + i + "_mode"]);
            entry.players = String(values["s" + i + "_players"]);
            entry.maxPlayers = String(values["s" + i + "_maxPlayers"]);
            entry.ping = String(values["s" + i + "_ping"]);
            entry.locked = String(values["s" + i + "_locked"]);
            entry.mapId = String(values["s" + i + "_mapId"]);
            entry.mapName = String(values["s" + i + "_mapName"]);
            entries.push(entry);
            i++;
         }
         if(this.onServerList != null)
         {
            this.onServerList(entries);
         }
      }
      
      private function send(type:String, values:Object) : void
      {
         var line:String = type;
         for(var key in values)
         {
            line += "|" + key + "=" + encodeValue(String(values[key]));
         }
         line += "\n";
         if(!this._connected || this._socket == null || !this._socket.connected)
         {
            this._queue.push(line);
            this.connect();
            return;
         }
         this._socket.writeUTFBytes(line);
         this._socket.flush();
      }
      
      private function flushQueue() : void
      {
         while(this._queue.length > 0 && this._socket != null && this._socket.connected)
         {
            this._socket.writeUTFBytes(String(this._queue.shift()));
         }
         if(this._socket != null && this._socket.connected)
         {
            this._socket.flush();
         }
      }
      
      private function invokeStatus(message:String) : void
      {
         if(this.onStatus != null)
         {
            this.onStatus(message);
         }
      }
      
      private function invokeError(message:String) : void
      {
         if(this.onError != null)
         {
            this.onError(message);
         }
         else
         {
            this.invokeStatus(message);
         }
      }
   }
}
