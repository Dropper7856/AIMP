package frontend
{
   import flash.events.Event;
   import flash.events.IOErrorEvent;
   import flash.events.ProgressEvent;
   import flash.events.SecurityErrorEvent;
   import flash.events.TimerEvent;
   import flash.net.Socket;
   import flash.utils.Timer;
   import frontend.enum.MenuPageId;
   import frontend.events.MenuItemEvent;
   import frontend.menu.MenuItem;
   import frontend.menu.MenuItemDataSequence;
   import frontend.menu.MenuPage;

   public class PauseMenu extends GameMenu
   {


      private var _debugMenuItem:MenuItem;

      private var _resumeGameItem:MenuItem;

      private var _challengeOrPreorderEnabled:Boolean;

      private var _aimpSocket:Socket;

      private var _aimpBuffer:String = "";

      private var _aimpMultiplayer:Boolean = false;

      private var _aimpDisconnectItem:MenuItem;

      private var _aimpQuitTimer:Timer;

      private var _aimpStateTimer:Timer;

      private var _aimpDisconnectPending:Boolean = false;

      private var _aimpDisconnectRequest:uint = 0;

      public function PauseMenu()
      {
         this._debugMenuItem = new MenuItem("Debug Menu",this.debugMenuHandler);
         this._resumeGameItem = new MenuItem("AI_UI_PAUSE_RESUME_GAME",this.resumeGameHandler);
         super("");
         var _loc1_:MenuPage = new MenuPage();
         _loc1_.id = MenuPageId.ROOT;
         _loc1_.addItem(this._debugMenuItem);
         _loc1_.addItem(this._resumeGameItem);
         _loc1_.addItem(_loadGameItem);
         _loc1_.addItem(_optionsItem);
         _loc1_.addItem(_helpItem);
         _loc1_.addItem(_backToFrontendItem);
         _loc1_.addItem(_quitGameItem);
         extras.backButtonCallback = this.backButtonCallback;
         menu.extrasTopLevel = true;
         menu.additionalDescription = description;
         menu.additionalTransitions = transitions;
         menu.extras = extras;
         menu.rootPage = _loc1_;
         this.__setProp_description_Scene1_description_0();
         this.__setProp_userProfilePanel_Scene1_profile_0();
         this._aimpDisconnectItem = new MenuItem("Disconnect",this.aimpDisconnectHandler);
         this._aimpDisconnectItem.description = "Leave the multiplayer game and return to the main menu";
         this._aimpStateTimer = new Timer(1000);
         this._aimpStateTimer.addEventListener(TimerEvent.TIMER,this.aimpRefreshState);
         addEventListener(Event.ADDED_TO_STAGE,this.aimpAddedToStage);
         addEventListener(Event.REMOVED_FROM_STAGE,this.aimpRemovedFromStage);
         this._aimpStateTimer.start();
         this.aimpConnectBridge();
      }

      private function aimpAddedToStage(param1:Event) : void
      {
         this._aimpStateTimer.start();
         this.aimpRefreshState(null);
      }

      private function aimpRemovedFromStage(param1:Event) : void
      {
         this._aimpStateTimer.stop();
         this.aimpStopQuitTimer();
         this._aimpDisconnectPending = false;
         try
         {
            if(this._aimpSocket != null)
            {
               this._aimpSocket.close();
            }
         }
         catch(error:Error)
         {
         }
         this._aimpSocket = null;
      }

      private function aimpRefreshState(param1:TimerEvent) : void
      {
         if(this._aimpDisconnectPending)
         {
            return;
         }
         if(this._aimpSocket == null)
         {
            this.aimpConnectBridge();
         }
         else
         {
            this.aimpSend("PAUSE_MENU_HELLO|source=pause_menu|phase=pause");
         }
      }

      private function aimpConnectBridge() : void
      {
         try
         {
            this._aimpSocket = new Socket();
            this._aimpSocket.addEventListener(Event.CONNECT,this.aimpBridgeConnected);
            this._aimpSocket.addEventListener(ProgressEvent.SOCKET_DATA,this.aimpBridgeData);
            this._aimpSocket.addEventListener(IOErrorEvent.IO_ERROR,this.aimpBridgeError);
            this._aimpSocket.addEventListener(SecurityErrorEvent.SECURITY_ERROR,this.aimpBridgeError);
            this._aimpSocket.addEventListener(Event.CLOSE,this.aimpBridgeError);
            this._aimpSocket.connect("127.0.0.1",12346);
         }
         catch(error:Error)
         {
            this._aimpSocket = null;
         }
      }

      private function aimpBridgeConnected(param1:Event) : void
      {
         this.aimpSend("PAUSE_MENU_HELLO|source=pause_menu|phase=pause");
      }

      private function aimpBridgeError(param1:Event) : void
      {
         if(param1.currentTarget != this._aimpSocket)
         {
            return;
         }
         this._aimpSocket = null;
      }

      private function aimpSend(param1:String) : Boolean
      {
         try
         {
            if(this._aimpSocket != null && this._aimpSocket.connected)
            {
               this._aimpSocket.writeUTFBytes(param1 + "\n");
               this._aimpSocket.flush();
               return true;
            }
         }
         catch(error:Error)
         {
         }
         return false;
      }

      private function aimpBridgeData(param1:ProgressEvent) : void
      {
         var _loc2_:int = 0;
         try
         {
            this._aimpBuffer += this._aimpSocket.readUTFBytes(this._aimpSocket.bytesAvailable);
            _loc2_ = this._aimpBuffer.indexOf("\n");
            while(_loc2_ >= 0)
            {
               this.aimpHandleLine(this._aimpBuffer.substr(0,_loc2_).split("\r").join(""));
               this._aimpBuffer = this._aimpBuffer.substr(_loc2_ + 1);
               _loc2_ = this._aimpBuffer.indexOf("\n");
            }
         }
         catch(error:Error)
         {
         }
      }

      private function aimpHandleLine(param1:String) : void
      {
         if(param1.indexOf("PAUSE_MENU_DISCONNECT_READY|") == 0)
         {
            if(!this._aimpDisconnectPending || this.aimpGetValue(param1,"requestId") != String(this._aimpDisconnectRequest))
            {
               return;
            }
            this.aimpStopQuitTimer();
            this._aimpDisconnectPending = false;
            if(this.aimpGetValue(param1,"success") == "1")
            {
               call("back_to_frontend");
            }
            else
            {
               this.aimpDisconnectFailed(null);
            }
            return;
         }
         if(param1.indexOf("PAUSE_MENU_STATE") != 0)
         {
            return;
         }
         this.aimpSetMultiplayer(param1.indexOf("|multiplayer=1") >= 0);
      }

      private function aimpGetValue(param1:String, param2:String) : String
      {
         var _loc3_:Array = param1.split("|");
         var _loc4_:String = param2 + "=";
         var _loc5_:String = null;
         for each(_loc5_ in _loc3_)
         {
            if(_loc5_.indexOf(_loc4_) == 0)
            {
               return _loc5_.substr(_loc4_.length);
            }
         }
         return "";
      }

      private function aimpSetMultiplayer(param1:Boolean) : void
      {
         var _loc2_:int = 0;
         if(this._aimpMultiplayer == param1)
         {
            return;
         }
         this._aimpMultiplayer = param1;
         if(param1)
         {
            _loc2_ = menu.rootPage.items.indexOf(_backToFrontendItem);
            if(_loc2_ < 0)
            {
               _loc2_ = menu.rootPage.items.length;
            }
            menu.rootPage.removeItem(_backToFrontendItem);
            menu.rootPage.addItemAtIndex(this._aimpDisconnectItem,_loc2_);
         }
         else
         {
            _loc2_ = menu.rootPage.items.indexOf(this._aimpDisconnectItem);
            menu.rootPage.removeItem(this._aimpDisconnectItem);
            menu.rootPage.addItemAtIndex(_backToFrontendItem,_loc2_ < 0 ? menu.rootPage.items.length : _loc2_);
         }
         menu.rebuildItemMap();
         menu.rebuildPageMap();
         menu.refresh();
      }

      private function aimpDisconnectHandler(param1:MenuItemEvent) : void
      {
         if(this._aimpDisconnectPending)
         {
            return;
         }
         this._aimpDisconnectRequest++;
         this._aimpDisconnectPending = true;
         if(!this.aimpSend("PAUSE_MENU_DISCONNECT|source=pause_menu|phase=pause|requestId=" + this._aimpDisconnectRequest))
         {
            this.aimpDisconnectFailed(null);
            return;
         }
         this._aimpDisconnectItem.description = "Disconnecting...";
         this._aimpQuitTimer = new Timer(15000,1);
         this._aimpQuitTimer.addEventListener(TimerEvent.TIMER_COMPLETE,this.aimpDisconnectFailed);
         this._aimpQuitTimer.start();
         menu.refresh();
      }

      private function aimpStopQuitTimer() : void
      {
         if(this._aimpQuitTimer != null)
         {
            this._aimpQuitTimer.stop();
            this._aimpQuitTimer.removeEventListener(TimerEvent.TIMER_COMPLETE,this.aimpDisconnectFailed);
            this._aimpQuitTimer = null;
         }
      }

      private function aimpDisconnectFailed(param1:TimerEvent) : void
      {
         this.aimpStopQuitTimer();
         this._aimpDisconnectPending = false;
         this._aimpDisconnectItem.description = "Unable to disconnect. Please try again.";
         menu.refresh();
      }

      override protected function imageSubstitutionLoadedCallback() : void
      {
         super.imageSubstitutionLoadedCallback();
      }

      private function backButtonCallback() : Boolean
      {
         if(menu.isTopLevel)
         {
            call("resume_game");
            return true;
         }
         return false;
      }

      public function setDebugMenuItemVisible(param1:Boolean) : void
      {
         this._debugMenuItem.visible = false;
      }

      public function setChallengeOrPreorderEnabled(param1:Boolean) : void
      {
         if(this._challengeOrPreorderEnabled != param1)
         {
            this._challengeOrPreorderEnabled = param1;
            if(this._challengeOrPreorderEnabled)
            {
               menu.rootPage.addItemAtIndex(_currentCheckpointItem,2);
               menu.rootPage.addItemAtIndex(_previousCheckpointItem,3);
            }
            else
            {
               menu.rootPage.removeItem(_currentCheckpointItem);
               menu.rootPage.removeItem(_previousCheckpointItem);
            }
            menu.rebuildItemMap();
            menu.rebuildPageMap();
            menu.refresh();
         }
      }

      private function debugMenuHandler(param1:MenuItemEvent) : void
      {
         call("show_debug_menu");
      }

      private function resumeGameHandler(param1:MenuItemEvent) : void
      {
         call("resume_game");
      }

      private function signinHandler(param1:MenuItemEvent) : void
      {
         call("signin");
      }

      override protected function setIds() : void
      {
         super.setIds();
      }

      override protected function setDescriptions() : void
      {
         super.setDescriptions();
         this._debugMenuItem.description = "Open the debug menu";
         this._resumeGameItem.description = "AI_UI_PAUSE_RESUME_GAME_DESC";
      }

      override protected function setTransitions() : void
      {
         super.setTransitions();
      }

      override protected function setVisibility() : void
      {
         super.setVisibility();
         this._debugMenuItem.visible = false;
      }

      override public function setAudioLanguageData(param1:MenuItemDataSequence) : void
      {
         super.setAudioLanguageData(param1);
         _audioLanguageItem.visible = false;
      }

      internal function __setProp_description_Scene1_description_0() : *
      {
         try
         {
            description["componentInspectorSetting"] = true;
         }
         catch(e:Error)
         {
         }
         description.transitionInEnabled = true;
         description.transitionInLabel = "transition_in";
         description.transitionOutEnabled = true;
         description.transitionOutLabel = "transition_out";
         description.transitionOwner = "self";
         description.visible = true;
         try
         {
            description["componentInspectorSetting"] = false;
            return;
         }
         catch(e:Error)
         {
            return;
         }
      }

      internal function __setProp_userProfilePanel_Scene1_profile_0() : *
      {
         try
         {
            userProfilePanel["componentInspectorSetting"] = true;
         }
         catch(e:Error)
         {
         }
         userProfilePanel.transitionInEnabled = false;
         userProfilePanel.transitionInLabel = "transition_in";
         userProfilePanel.transitionOutEnabled = false;
         userProfilePanel.transitionOutLabel = "transition_out";
         userProfilePanel.transitionOwner = "self";
         userProfilePanel.visible = false;
         try
         {
            userProfilePanel["componentInspectorSetting"] = false;
            return;
         }
         catch(e:Error)
         {
            return;
         }
      }
   }
}
