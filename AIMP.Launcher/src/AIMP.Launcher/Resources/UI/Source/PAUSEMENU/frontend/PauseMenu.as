package frontend
{
   import flash.events.Event;
   import flash.events.IOErrorEvent;
   import flash.events.ProgressEvent;
   import flash.events.SecurityErrorEvent;
   import flash.net.Socket;
   import flash.utils.getTimer;
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
      private var _aimpLines:Array = [];
      private var _aimpMultiplayer:Boolean = false;
      private var _aimpStateRequested:Boolean = false;
      private var _aimpStateKnown:Boolean = false;
      private var _aimpLeaveWhenKnown:Boolean = false;
      private var _aimpStateDeadline:int = 0;
      private var _aimpDisconnectPending:Boolean = false;
      private var _aimpDisconnectRequest:uint = 0;
      private var _aimpDisconnectDeadline:int = 0;
      private var _aimpRetryAt:int = 0;
      private var _aimpBridgeFailed:Boolean = false;
      private var _aimpExitRequested:Boolean = false;
      private var _aimpExitOnNextFrame:Boolean = false;

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
         addEventListener(Event.ADDED_TO_STAGE,this.aimpAddedToStage);
         addEventListener(Event.REMOVED_FROM_STAGE,this.aimpRemovedFromStage);
         addEventListener(Event.ENTER_FRAME,this.aimpFrame,false,0,true);
      }

      private function aimpAddedToStage(param1:Event) : void
      {
         this._aimpStateRequested = false;
         addEventListener(Event.ENTER_FRAME,this.aimpFrame,false,0,true);
      }

      private function aimpRemovedFromStage(param1:Event) : void
      {
         removeEventListener(Event.ENTER_FRAME,this.aimpFrame);
         this._aimpDisconnectPending = false;
         this._aimpLeaveWhenKnown = false;
         this._aimpExitRequested = false;
         this._aimpExitOnNextFrame = false;
         this.aimpCloseBridge();
      }

      private function aimpFrame(param1:Event) : void
      {
         // Wait for retail initialization. Socket callbacks only collect data;
         // menu changes and native navigation execute on this UI frame.
         if(stage == null || menu == null || !menu.initialized)
         {
            return;
         }
         if(this._aimpExitOnNextFrame)
         {
            removeEventListener(Event.ENTER_FRAME,this.aimpFrame);
            this._aimpExitOnNextFrame = false;
            call("back_to_frontend");
            return;
         }
         if(this._aimpBridgeFailed)
         {
            this._aimpBridgeFailed = false;
            this.aimpCloseBridge();
            this._aimpRetryAt = getTimer() + 3000;
            if(this._aimpDisconnectPending)
            {
               this.aimpDisconnectFailed();
            }
         }
         while(this._aimpLines.length > 0)
         {
            this.aimpHandleLine(String(this._aimpLines.shift()));
            if(this._aimpExitRequested)
            {
               this._aimpExitRequested = false;
               this.aimpCloseBridge();
               this._aimpExitOnNextFrame = true;
               return;
            }
         }
         if(this._aimpLeaveWhenKnown)
         {
            if(this._aimpStateKnown)
            {
               this._aimpLeaveWhenKnown = false;
               this.backToFrontendHandler(null);
            }
            else if(getTimer() - this._aimpStateDeadline >= 0)
            {
               this._aimpLeaveWhenKnown = false;
               _backToFrontendItem.description = "Unable to read session status. Please try again.";
               menu.refresh();
            }
         }
         if(this._aimpDisconnectPending && getTimer() - this._aimpDisconnectDeadline >= 0)
         {
            this.aimpDisconnectFailed();
         }
         if(this._aimpSocket == null)
         {
            if(getTimer() - this._aimpRetryAt >= 0)
            {
               this.aimpConnectBridge();
            }
         }
         else if(this._aimpSocket.connected && !this._aimpStateRequested)
         {
            this._aimpStateRequested = this.aimpSend("PAUSE_MENU_HELLO|source=pause_menu|phase=pause");
         }
      }

      private function aimpConnectBridge() : void
      {
         this._aimpRetryAt = getTimer() + 3000;
         this._aimpBuffer = "";
         this._aimpLines = [];
         try
         {
            this._aimpSocket = new Socket();
            this._aimpSocket.addEventListener(ProgressEvent.SOCKET_DATA,this.aimpBridgeData);
            this._aimpSocket.addEventListener(IOErrorEvent.IO_ERROR,this.aimpBridgeError);
            this._aimpSocket.addEventListener(SecurityErrorEvent.SECURITY_ERROR,this.aimpBridgeError);
            this._aimpSocket.addEventListener(Event.CLOSE,this.aimpBridgeError);
            this._aimpSocket.connect("127.0.0.1",12346);
         }
         catch(error:Error)
         {
            this.aimpCloseBridge();
         }
      }

      private function aimpCloseBridge() : void
      {
         var _loc1_:Socket = this._aimpSocket;
         this._aimpSocket = null;
         this._aimpStateRequested = false;
         this._aimpStateKnown = false;
         this._aimpBuffer = "";
         this._aimpLines = [];
         if(_loc1_ == null)
         {
            return;
         }
         _loc1_.removeEventListener(ProgressEvent.SOCKET_DATA,this.aimpBridgeData);
         _loc1_.removeEventListener(IOErrorEvent.IO_ERROR,this.aimpBridgeError);
         _loc1_.removeEventListener(SecurityErrorEvent.SECURITY_ERROR,this.aimpBridgeError);
         _loc1_.removeEventListener(Event.CLOSE,this.aimpBridgeError);
         try
         {
            _loc1_.close();
         }
         catch(error:Error)
         {
         }
      }

      private function aimpBridgeError(param1:Event) : void
      {
         if(param1.currentTarget != this._aimpSocket)
         {
            return;
         }
         this._aimpBridgeFailed = true;
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
            this.aimpCloseBridge();
         }
         return false;
      }

      private function aimpBridgeData(param1:ProgressEvent) : void
      {
         var _loc2_:Socket = param1.currentTarget as Socket;
         var _loc3_:int = 0;
         if(_loc2_ == null || _loc2_ != this._aimpSocket)
         {
            return;
         }
         try
         {
            this._aimpBuffer += _loc2_.readUTFBytes(_loc2_.bytesAvailable);
            if(this._aimpBuffer.length > 65536)
            {
               this.aimpBridgeError(param1);
               return;
            }
            _loc3_ = this._aimpBuffer.indexOf("\n");
            while(_loc3_ >= 0)
            {
               if(this._aimpLines.length >= 16)
               {
                  this.aimpBridgeError(param1);
                  return;
               }
               this._aimpLines.push(this._aimpBuffer.substr(0,_loc3_).split("\r").join(""));
               this._aimpBuffer = this._aimpBuffer.substr(_loc3_ + 1);
               _loc3_ = this._aimpBuffer.indexOf("\n");
            }
         }
         catch(error:Error)
         {
            this.aimpBridgeError(param1);
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
            this._aimpDisconnectPending = false;
            if(this.aimpGetValue(param1,"success") == "1")
            {
               this._aimpExitRequested = true;
            }
            else
            {
               this.aimpDisconnectFailed();
            }
         }
         else if(param1.indexOf("PAUSE_MENU_STATE|") == 0)
         {
            this.aimpSetMultiplayer(this.aimpGetValue(param1,"multiplayer") == "1");
            this._aimpStateKnown = true;
         }
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
         if(this._aimpMultiplayer == param1)
         {
            return;
         }
         this._aimpMultiplayer = param1;
         // Preserve the retail BACK_TO_FRONTEND id, bindings and page entry.
         _backToFrontendItem.name = param1 ? "Disconnect" : "AI_UI_PAUSE_TO_FRONTEND";
         _backToFrontendItem.description = param1 ? "Leave the multiplayer game and return to the main menu" : "AI_UI_PAUSE_TO_FRONTEND_DESC";
         menu.refresh();
      }

      override protected function backToFrontendHandler(param1:MenuItemEvent) : void
      {
         if(!this._aimpStateKnown)
         {
            this._aimpLeaveWhenKnown = true;
            this._aimpStateDeadline = getTimer() + 15000;
            _backToFrontendItem.description = "Waiting for session status...";
            menu.refresh();
            return;
         }
         if(!this._aimpMultiplayer)
         {
            super.backToFrontendHandler(param1);
            return;
         }
         if(this._aimpDisconnectPending)
         {
            return;
         }
         this._aimpDisconnectRequest++;
         this._aimpDisconnectPending = true;
         if(!this.aimpSend("PAUSE_MENU_DISCONNECT|source=pause_menu|phase=pause|requestId=" + this._aimpDisconnectRequest))
         {
            this.aimpDisconnectFailed();
            return;
         }
         this._aimpDisconnectDeadline = getTimer() + 15000;
         _backToFrontendItem.description = "Disconnecting...";
         menu.refresh();
      }

      private function aimpDisconnectFailed() : void
      {
         this._aimpDisconnectPending = false;
         _backToFrontendItem.description = "Unable to disconnect. Please try again.";
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
