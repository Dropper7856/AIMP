package
{
   import flash.display.*;
   import flash.events.*;
   import flash.external.*;
   import flash.net.Socket;
   import flash.utils.*;
   import leaderboards.*;
   import utils.*;
   
   public class ChallengeMapOverlay extends MovieClip
   {
       
      
      public var salvageComplete_mc:MovieClip;
      
      public var gauntletProgressPopup_mc:MovieClip;
      
      public var continuePrompt_mc:MovieClip;
      
      public var timerHeader:MovieClip;
      
      public var pointsAward_mc:MovieClip;
      
      public var obj2_mc:MovieClip;
      
      public var obj3_mc:MovieClip;
      
      public var obj1_mc:MovieClip;
      
      public var fader:MovieClip;
      
      public var timelimit_mc:MovieClip;
      
      public var playlistEndResults_mc:MovieClip;
      
      public var leaderboard_loading_spinner_mc:MovieClip;
      
      public var progressContinue_mc:MovieClip;
      
      public var marathonProgressPopup_mc:MovieClip;
      
      public var salvageAbandoned_mc:MovieClip;
      
      public var trigger_mc:MovieClip;
      
      public var completedScreen_mc:MovieClip;
      
      public var waveHistory:MovieClip;
      
      public var titlecard_mc:MovieClip;
      
      public var obj1Icon:MovieClip;
      
      public var salvageTimer_mc:MovieClip;
      
      public var obj2Icon:MovieClip;
      
      public var obj3Icon:MovieClip;
      
      internal var time:Number = 0;
      
      internal var obj1:Number = 0;
      
      internal var obj2:Number = 0;
      
      internal var obj3:Number = 0;
      
      internal var obj1Decay:* = 3;
      
      internal var obj2Decay:* = 2;
      
      internal var obj3Decay:* = 1;
      
      internal var finalScore:int = 0;
      
      internal var miscPoints:* = 0;
      
      internal var obj1Complete:* = false;
      
      internal var obj2Complete:* = false;
      
      internal var obj3Complete:* = false;
      
      internal var challengeID:* = 0;
      
      internal var obj1symbol:* = "idtags";
      
      internal var obj2symbol:* = "idtags";
      
      internal var obj3symbol:* = "idtags";
      
      internal var multTotal:int = 1;
      
      internal var packPrefix:String = "~AI_DLC";
      
      internal var m_leaderboard_spinner_visible:Boolean = false;
      
      public var leaderboard_library:LeaderBoardLib;
      
      public var leaderboard_holder_mc:MovieClip;
      
      public var connection_popup_mc:MovieClip;
      
      internal var keyboard_input:Boolean = false;
      
      internal var m_targetting_leaderboard:Boolean = false;
      
      private var m_layer_name:String = "ChallengeMapOverlay";
      
      private var aimp_launcher_socket:Socket;
      
      private var aimp_launcher_connected:Boolean = false;
      
      private var aimp_launcher_queue:Array;
      
      internal var leaderboard_request_timer:Timer;
      
      private var objectiveDecay:Array;
      
      private var objectiveIcon:Array;
      
      private var objectiveValue:Array;
      
      internal var award1Complete:* = true;
      
      internal var award2Complete:* = true;
      
      internal var award3Complete:* = true;
      
      internal var award4Complete:* = true;
      
      internal var award5Complete:* = false;
      
      internal var award6Complete:* = true;
      
      internal var award7Complete:* = true;
      
      internal var award8Complete:* = false;
      
      internal var TotalMultiplier:* = 0;
      
      internal var isSalvageMap:* = false;
      
      internal var salvageCurrentWave:* = 0;
      
      internal var salvageCurrentChoice:* = 0;
      
      internal var salvageCurrentObjScore:* = 0;
      
      internal var salvageCurrentObjDecay:* = 0;
      
      internal var salvageTotalScore:* = 0;
      
      internal var salvageMissionActive:* = false;
      
      internal var salvageMapCompleted:* = false;
      
      public var substitution:ImageSubstitution;
      
      public function ChallengeMapOverlay()
      {
         this.aimp_launcher_queue = [];
         this.leaderboard_library = new LeaderBoardLib(this);
         this.leaderboard_request_timer = new Timer(1000,0);
         this.objectiveDecay = [2,1,2,1,1,1,1,1,1,2,2,3,2,2,1,1,2,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1];
         this.objectiveIcon = [6,3,2,3,5,4,1,1,1,3,5,4,5,1,4,3,2,4,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1];
         this.objectiveValue = [7500,7000,5000,1,1,1,1,1,1,9750,3500,5250,7500,7000,9750,5250,5000,11250,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1];
         this.substitution = new ImageSubstitution();
         super();
         addFrameScript(0,this.frame1,1,this.frame2,83,this.frame84,122,this.frame123,162,this.frame163,203,this.frame204,208,this.frame209,209,this.frame210,210,this.frame211,227,this.frame228,228,this.frame229,237,this.frame238,246,this.frame247,251,this.frame252,252,this.frame253,255,this.frame256,260,this.frame261,261,this.frame262,267,this.frame268);
         this.leaderboard_library.loadedCallback = this.leaderboard_loaded_callback;
      }
      
      public function salvageResetStart() : *
      {
         gotoAndPlay("SalvageHud");
         this.leaderboard_request_timer.addEventListener(TimerEvent.TIMER,this.OnRequestCallback);
         this.salvageCurrentWave = 0;
         this.salvageCurrentChoice = 0;
         this.salvageCurrentObjScore = 0;
         this.salvageCurrentObjDecay = 0;
         this.salvageTotalScore = 0;
         this.time = 0;
         thefinaltime = 0;
         this.isSalvageMap = true;
         this.salvageTimer_mc.visible = true;
         this.timelimit_mc.visible = true;
         this.waveHistory.visible = true;
         this.progressContinue_mc.visible = false;
         this.continuePrompt_mc.visible = false;
         this.titlecard_mc.visible = false;
         this.salvageTimer_mc.gotoAndPlay("idle");
         this.salvageTimer_mc.message_txt.txt.text = "AI_UI_TIMER_GET_READY";
         this.salvageTimer_mc.score_message_txt.txt.text = "AI_UI_CHALLENGE_END_FSCORE";
         this.salvageTimer_mc.score_mc.txt.text = this.salvageTotalScore;
         this.salvageTimer_mc.timer_mc.txt.text = this.time;
         this.salvageMissionActive = false;
         this.resetWaveHud();
      }
      
      public function beginSalvageTimer() : *
      {
         this.timelimit_mc.visible = false;
         this.salvageTimer_mc.gotoAndPlay("begin");
         this.salvageTimer_mc.message_txt.txt.text = "AI_UI_TIMER_TIME_ELAPSED";
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","Play_Timer_Start");
         }
      }
      
      public function setCurrentWaveInfo(param1:int, param2:int, param3:int) : *
      {
         this.salvageCurrentWave = param1;
         this.salvageCurrentObjScore = param2;
         this.salvageCurrentObjDecay = param3;
      }
      
      public function resetWaveHud() : void
      {
         if(this.waveHistory)
         {
            this.waveHistory.icon1.gotoAndStop(0);
            this.waveHistory.icon2.gotoAndStop(0);
            this.waveHistory.icon3.gotoAndStop(0);
            this.waveHistory.icon4.gotoAndStop(0);
            this.waveHistory.icon5.gotoAndStop(0);
            this.waveHistory.icon6.gotoAndStop(0);
            this.waveHistory.icon7.gotoAndStop(0);
            this.waveHistory.icon8.gotoAndStop(0);
            this.waveHistory.icon9.gotoAndStop(0);
            this.waveHistory.icon10.gotoAndStop(0);
            this.waveHistory.gotoAndStop(0);
            this.waveHistory.icon1.score.txt.text = 0;
            this.waveHistory.icon2.score.txt.text = 0;
            this.waveHistory.icon3.score.txt.text = 0;
            this.waveHistory.icon4.score.txt.text = 0;
            this.waveHistory.icon5.score.txt.text = 0;
            this.waveHistory.icon6.score.txt.text = 0;
            this.waveHistory.icon7.score.txt.text = 0;
            this.waveHistory.icon8.score.txt.text = 0;
            this.waveHistory.icon9.score.txt.text = 0;
            this.waveHistory.icon10.score.txt.text = 0;
         }
      }
      
      public function setCurrentWaveHUD(param1:int) : *
      {
         if(this.salvageCurrentWave == 1)
         {
            this.waveHistory.icon1.gotoAndStop(param1);
         }
         else if(this.salvageCurrentWave == 2)
         {
            this.waveHistory.icon2.gotoAndStop(param1);
         }
         else if(this.salvageCurrentWave == 3)
         {
            this.waveHistory.icon3.gotoAndStop(param1);
         }
         else if(this.salvageCurrentWave == 4)
         {
            this.waveHistory.icon4.gotoAndStop(param1);
         }
         else if(this.salvageCurrentWave == 5)
         {
            this.waveHistory.icon5.gotoAndStop(param1);
         }
         else if(this.salvageCurrentWave == 6)
         {
            this.waveHistory.icon6.gotoAndStop(param1);
         }
         else if(this.salvageCurrentWave == 7)
         {
            this.waveHistory.icon7.gotoAndStop(param1);
         }
         else if(this.salvageCurrentWave == 8)
         {
            this.waveHistory.icon8.gotoAndStop(param1);
         }
         else if(this.salvageCurrentWave == 9)
         {
            this.waveHistory.icon9.gotoAndStop(param1);
         }
         else if(this.salvageCurrentWave == 10)
         {
            this.waveHistory.icon10.gotoAndStop(param1);
         }
         else if(this.salvageCurrentWave == 0)
         {
            this.waveHistory.gotoAndStop("error");
         }
         else
         {
            this.waveHistory.gotoAndStop("error");
         }
         this.waveHistory.gotoAndStop(this.salvageCurrentWave);
         if(this.salvageCurrentWave == 1)
         {
            this.waveHistory.icon1.score.txt.text = this.salvageCurrentObjScore;
         }
         else if(this.salvageCurrentWave == 2)
         {
            this.waveHistory.icon2.score.txt.text = this.salvageCurrentObjScore;
         }
         else if(this.salvageCurrentWave == 3)
         {
            this.waveHistory.icon3.score.txt.text = this.salvageCurrentObjScore;
         }
         else if(this.salvageCurrentWave == 4)
         {
            this.waveHistory.icon4.score.txt.text = this.salvageCurrentObjScore;
         }
         else if(this.salvageCurrentWave == 5)
         {
            this.waveHistory.icon5.score.txt.text = this.salvageCurrentObjScore;
         }
         else if(this.salvageCurrentWave == 6)
         {
            this.waveHistory.icon6.score.txt.text = this.salvageCurrentObjScore;
         }
         else if(this.salvageCurrentWave == 7)
         {
            this.waveHistory.icon7.score.txt.text = this.salvageCurrentObjScore;
         }
         else if(this.salvageCurrentWave == 8)
         {
            this.waveHistory.icon8.score.txt.text = this.salvageCurrentObjScore;
         }
         else if(this.salvageCurrentWave == 9)
         {
            this.waveHistory.icon9.score.txt.text = this.salvageCurrentObjScore;
         }
         else if(this.salvageCurrentWave == 10)
         {
            this.waveHistory.icon10.score.txt.text = this.salvageCurrentObjScore;
         }
      }
      
      public function finishObjective() : *
      {
         this.salvageTotalScore += this.salvageCurrentObjScore;
         this.salvageTimer_mc.score_mc.txt.text = this.salvageTotalScore;
         this.GenericCallbackInt("waveCompleteScore",int(this.salvageCurrentObjScore));
         this.GenericCallbackInt("newTotalScore",int(this.salvageTotalScore));
      }
      
      public function checkScore() : *
      {
         this.GenericCallbackInt("currentScore",int(this.salvageTotalScore));
      }
      
      public function applyMiscPoints(param1:int) : *
      {
         this.salvageTotalScore += param1;
         this.salvageTimer_mc.score_mc.txt.text = this.salvageTotalScore;
         this.pointsAward_mc.text_mc.txt.text = "+ " + param1;
         this.pointsAward_mc.gotoAndPlay("trans_in");
      }
      
      public function adjustScore(param1:int) : *
      {
         this.salvageTotalScore += param1;
         this.salvageTimer_mc.score_mc.txt.text = this.salvageTotalScore;
         this.pointsAward_mc.text_mc.txt.text = param1;
      }
      
      public function activateMission() : *
      {
         this.timelimit_mc.visible = false;
         this.salvageMissionActive = true;
      }
      
      public function deactivateMission() : *
      {
         this.salvageMissionActive = false;
      }
      
      public function salvageCompleted() : *
      {
         this.salvageMapCompleted = true;
      }
      
      public function salvageAbandoned() : *
      {
         this.salvageMapCompleted = false;
      }
      
      public function salvageEndScreen() : *
      {
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","Play_Timer_Stop");
         }
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","Play_Score_Multiplier");
         }
         gotoAndPlay("SalvageEnd");
         this.salvageTimer_mc.visible = false;
         this.waveHistory.visible = false;
         if(this.salvageMapCompleted)
         {
            this.salvageComplete_mc.visible = true;
            this.salvageAbandoned_mc.visible = false;
            this.salvageComplete_mc.gotoAndPlay("trans_in");
            this.salvageComplete_mc.finalScoreEND_mc.txt.text = this.salvageTotalScore;
            this.salvageComplete_mc.finalTimeEND_mc.txt.text = StringUtil.millisecondsToTime(this.time * 1000,":",false,true);
         }
         else if(!this.salvageMapCompleted)
         {
            this.salvageAbandoned_mc.visible = true;
            this.salvageComplete_mc.visible = false;
            this.salvageAbandoned_mc.gotoAndPlay("trans_in");
            this.salvageAbandoned_mc.finalScoreEND_mc.txt.text = this.salvageTotalScore;
         }
         this.leaderboard_library.set_leaderboard_holder(this.leaderboard_holder_mc);
         this.leaderboard_library.set_leaderboard_level(this.leaderboard_library.get_current_level());
         ExternalInterface.call("RegisterInputListener",this.m_layer_name,ButtonCode.Y_ALT);
         this.target_leaderboard();
         stage.addEventListener(KeyboardEvent.KEY_DOWN,this.inputHandler);
         this.submit_timed_leaderboard_request();
         this.GenericCallbackInt("finalScore",int(this.salvageTotalScore));
         this.GenericCallbackFloat("finalTime",Number(this.time * 1000));
      }
      
      public function resizeListener() : *
      {
         var _loc1_:Number = 1280;
         var _loc2_:Number = 720;
         var _loc3_:Number = stage.stageWidth;
         var _loc4_:Number = stage.stageHeight;
         var _loc5_:Number = _loc3_ / _loc1_;
         var _loc6_:Number = _loc4_ / _loc2_;
         var _loc7_:Number = _loc5_;
         if(_loc6_ < _loc7_)
         {
            _loc7_ = _loc6_;
         }
         stage.scaleMode = StageScaleMode.NO_SCALE;
         stage.align = StageAlign.TOP_LEFT;
         var _loc8_:Number = 800;
         var _loc9_:Number = 100;
         this.titlecard_mc.width = _loc8_ * _loc7_;
         this.titlecard_mc.height = _loc9_ * _loc7_;
         this.titlecard_mc.x = 90 * _loc7_;
         this.titlecard_mc.y = stage.stageHeight - 170 * _loc7_;
         var _loc10_:Number = 495;
         var _loc11_:Number = 180;
         this.obj1Icon.width = _loc10_ * _loc7_;
         this.obj1Icon.height = _loc11_ * _loc7_;
         this.obj1Icon.x = 90 * _loc7_;
         this.obj1Icon.y = stage.stageHeight - 170 * _loc7_;
         var _loc12_:Number = 495;
         var _loc13_:Number = 180;
         this.obj2Icon.width = _loc12_ * _loc7_;
         this.obj2Icon.height = _loc13_ * _loc7_;
         this.obj2Icon.x = 90 * _loc7_;
         this.obj2Icon.y = stage.stageHeight - 170 * _loc7_;
         var _loc14_:Number = 495;
         var _loc15_:Number = 180;
         this.obj3Icon.width = _loc14_ * _loc7_;
         this.obj3Icon.height = _loc15_ * _loc7_;
         this.obj3Icon.x = 90 * _loc7_;
         this.obj3Icon.y = stage.stageHeight - 170 * _loc7_;
         var _loc16_:Number = 418;
         var _loc17_:Number = 23;
         this.obj1_mc.width = _loc16_ * _loc7_;
         this.obj1_mc.height = _loc17_ * _loc7_;
         this.obj1_mc.x = stage.stageWidth - 70 * _loc7_;
         this.obj1_mc.y = 42 * _loc7_;
         var _loc18_:Number = 418;
         var _loc19_:Number = 23;
         this.obj2_mc.width = _loc18_ * _loc7_;
         this.obj2_mc.height = _loc19_ * _loc7_;
         this.obj2_mc.x = stage.stageWidth - 70 * _loc7_;
         this.obj2_mc.y = 68 * _loc7_;
         var _loc20_:Number = 418;
         var _loc21_:Number = 23;
         this.obj3_mc.width = _loc20_ * _loc7_;
         this.obj3_mc.height = _loc21_ * _loc7_;
         this.obj3_mc.x = stage.stageWidth - 70 * _loc7_;
         this.obj3_mc.y = 95 * _loc7_;
         var _loc22_:Number = 195;
         var _loc23_:Number = 30;
         this.trigger_mc.width = _loc22_ * _loc7_;
         this.trigger_mc.height = _loc23_ * _loc7_;
         this.trigger_mc.x = 75 * _loc7_;
         this.trigger_mc.y = 87 * _loc7_;
         var _loc24_:Number = 150;
         var _loc25_:Number = 24;
         this.timerHeader.width = _loc24_ * _loc7_;
         this.timerHeader.height = _loc25_ * _loc7_;
         this.timerHeader.x = 75 * _loc7_;
         this.timerHeader.y = 55 * _loc7_;
         var _loc26_:Number = 48;
         var _loc27_:Number = 48;
         this.leaderboard_loading_spinner_mc.width = _loc26_ * _loc7_;
         this.leaderboard_loading_spinner_mc.height = _loc27_ * _loc7_;
         this.leaderboard_loading_spinner_mc.x = 825 * _loc7_;
         this.leaderboard_loading_spinner_mc.y = 321 * _loc7_;
         this.leaderboard_holder_mc.x = 621 * _loc7_;
         this.leaderboard_holder_mc.y = 184 * _loc7_;
         this.leaderboard_library.refresh_leaderboard_position_and_scale(_loc7_);
         var _loc28_:Number = 360;
         var _loc29_:Number = 388;
         this.completedScreen_mc.width = _loc28_ * _loc7_;
         this.completedScreen_mc.height = _loc29_ * _loc7_;
         this.completedScreen_mc.x = 248 * _loc7_;
         this.completedScreen_mc.y = 148 * _loc7_;
         var _loc30_:Number = 1112;
         var _loc31_:Number = 68;
         this.continuePrompt_mc.width = _loc30_ * _loc7_;
         this.continuePrompt_mc.height = _loc31_ * _loc7_;
         this.continuePrompt_mc.x = stage.stageWidth / 2;
         this.continuePrompt_mc.y = stage.stageHeight - 72 * _loc7_;
         var _loc32_:Number = 1112;
         var _loc33_:Number = 68;
         this.progressContinue_mc.width = _loc32_ * _loc7_;
         this.progressContinue_mc.height = _loc33_ * _loc7_;
         this.progressContinue_mc.x = stage.stageWidth / 2;
         this.progressContinue_mc.y = stage.stageHeight - 72 * _loc7_;
         var _loc34_:Number = 240;
         var _loc35_:Number = 23;
         this.connection_popup_mc.width = _loc34_ * _loc7_;
         this.connection_popup_mc.height = _loc35_ * _loc7_;
         this.connection_popup_mc.x = stage.stageWidth - 80 * _loc7_;
         this.connection_popup_mc.y = stage.stageHeight - 95 * _loc7_;
         var _loc36_:Number = 650;
         var _loc37_:Number = 285;
         this.gauntletProgressPopup_mc.width = _loc36_ * _loc7_;
         this.gauntletProgressPopup_mc.height = _loc37_ * _loc7_;
         this.gauntletProgressPopup_mc.x = stage.stageWidth / 2;
         this.gauntletProgressPopup_mc.y = stage.stageHeight / 2;
         var _loc38_:Number = 360;
         var _loc39_:Number = 390;
         this.playlistEndResults_mc.width = _loc38_ * _loc7_;
         this.playlistEndResults_mc.height = _loc39_ * _loc7_;
         this.playlistEndResults_mc.x = 248 * _loc7_;
         this.playlistEndResults_mc.y = 148 * _loc7_;
         var _loc40_:Number = 360;
         var _loc41_:Number = 390;
         this.salvageComplete_mc.width = _loc40_ * _loc7_;
         this.salvageComplete_mc.height = _loc41_ * _loc7_;
         this.salvageComplete_mc.x = 248 * _loc7_;
         this.salvageComplete_mc.y = 148 * _loc7_;
         var _loc42_:Number = 360;
         var _loc43_:Number = 390;
         this.salvageAbandoned_mc.width = _loc42_ * _loc7_;
         this.salvageAbandoned_mc.height = _loc43_ * _loc7_;
         this.salvageAbandoned_mc.x = 248 * _loc7_;
         this.salvageAbandoned_mc.y = 148 * _loc7_;
         var _loc44_:Number = 300;
         var _loc45_:Number = 100;
         this.salvageTimer_mc.width = _loc44_ * _loc7_;
         this.salvageTimer_mc.height = _loc45_ * _loc7_;
         this.salvageTimer_mc.x = 100 * _loc7_;
         this.salvageTimer_mc.y = 38 * _loc7_;
         var _loc46_:Number = 300;
         var _loc47_:Number = 30;
         this.timelimit_mc.width = _loc46_ * _loc7_;
         this.timelimit_mc.height = _loc47_ * _loc7_;
         this.timelimit_mc.x = 100 * _loc7_;
         this.timelimit_mc.y = 160 * _loc7_;
         var _loc48_:Number = 262;
         var _loc49_:Number = 230;
         this.waveHistory.width = _loc48_ * _loc7_;
         this.waveHistory.height = _loc49_ * _loc7_;
         this.waveHistory.x = stage.stageWidth - 60;
         this.waveHistory.y = 240 * _loc7_;
         var _loc50_:Number = 150;
         var _loc51_:Number = 50;
         this.pointsAward_mc.width = _loc50_ * _loc7_;
         this.pointsAward_mc.height = _loc51_ * _loc7_;
         this.pointsAward_mc.x = 358 * _loc7_;
         this.pointsAward_mc.y = 113 * _loc7_;
         var _loc52_:Number = 650;
         var _loc53_:Number = 85;
         this.marathonProgressPopup_mc.width = _loc52_ * _loc7_;
         this.marathonProgressPopup_mc.height = _loc53_ * _loc7_;
         this.marathonProgressPopup_mc.x = stage.stageWidth / 2;
         this.marathonProgressPopup_mc.y = stage.stageHeight / 2;
      }
      
      public function playObjSound() : *
      {
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","Play_Objective_Single");
         }
      }
      
      public function playAllObjSound() : *
      {
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","Play_All_Objectives");
         }
      }
      
      public function GenericCallbackInt(param1:String, param2:int) : void
      {
         if(ExternalInterface.available)
         {
            ExternalInterface.call("GenericCallbackInt",param1,param2);
         }
      }
      
      public function GenericCallbackFloat(param1:String, param2:Number) : void
      {
         if(ExternalInterface.available)
         {
            ExternalInterface.call("GenericCallbackFloat",param1,param2);
         }
      }
      
      public function init_image_substitution(param1:String, param2:Boolean, param3:Boolean) : void
      {
         if(param2)
         {
            this.continuePrompt_mc.continue_txt.text = "~AI_UI_TIP_LEADERBOARD_KB";
            this.progressContinue_mc.continue_txt.text = "~AI_UI_TIP_LEADERBOARD_EMPTYB";
         }
         else
         {
            this.continuePrompt_mc.continue_txt.text = "~AI_UI_TIP_LEADERBOARD";
            this.progressContinue_mc.continue_txt.text = "~AI_UI_TIP_LEADERBOARD_EMPTYB";
         }
         this.substitution.register_loaded_callback(this.substitution_loaded_callback);
         this.substitution.initialise(param1,param2,param3,ImageSubstitution.SUBSTITUTION_UI | ImageSubstitution.SUBSTITUTION_ACTIONS | ImageSubstitution.SUBSTITUTION_BUTTONS);
         if(param2 == true)
         {
            this.keyboard_input = true;
         }
         else
         {
            this.keyboard_input = false;
         }
      }
      
      private function substitution_loaded_callback() : void
      {
         trace("substitution loaded callback");
         this.substitution.Add_TextField(this.continuePrompt_mc.continue_txt);
         this.substitution.Add_TextField(this.connection_popup_mc.connection_sub_mc.txt);
         this.substitution.Add_TextField(this.progressContinue_mc.continue_txt);
         this.leaderboard_library.applyImageSubstitution(this.substitution);
      }
      
      private function leaderboard_loaded_callback() : void
      {
         this.leaderboard_library.applyImageSubstitution(this.substitution);
         this.leaderboard_library.set_focused(true);
         this.resizeListener();
      }
      
      public function pressed_view_player_info() : *
      {
         if(this.m_targetting_leaderboard)
         {
            trace("pressed_view_player_info");
            trace("m_targetting_leaderboard == true");
            this.leaderboard_library.pressed_view_player_info();
         }
      }
      
      public function pressed_changed_filter() : *
      {
         if(this.m_targetting_leaderboard)
         {
            trace("m_targetting_leaderboard == true");
            trace("pressed_changed_filter");
            this.leaderboard_library.pressed_changed_filter();
         }
      }
      
      public function pressed_continue() : *
      {
         if(this.m_targetting_leaderboard)
         {
            trace("m_targetting_leaderboard == true");
            trace("pressed_continue");
            if(ExternalInterface.available)
            {
               ExternalInterface.call("continue_requested",0);
            }
         }
      }
      
      public function pressed_continue_use() : *
      {
         if(Boolean(this.m_targetting_leaderboard) && this.progressContinue_mc.visible == true)
         {
            trace("m_targetting_leaderboard == true");
            trace("pressed_continue");
            if(ExternalInterface.available)
            {
               ExternalInterface.call("continue_requested",0);
            }
         }
      }
      
      public function populate_leaderboard(param1:Array, param2:Number, param3:Number) : *
      {
         if(ExternalInterface.available)
         {
            ExternalInterface.call("enable_leaderboard_input",0);
         }
         this.leaderboard_library.populate_leaderboard(param1,param2,param3);
         trace("test call");
      }
      
      public function transition_in_spinner() : *
      {
         trace("transition_in_spinner called");
         if(this.leaderboard_loading_spinner_mc)
         {
            this.m_leaderboard_spinner_visible = true;
            this.leaderboard_loading_spinner_mc.gotoAndPlay("trans_in");
         }
      }
      
      public function transition_out_spinner() : *
      {
         trace("transition_out_spinner called");
         if(Boolean(this.leaderboard_loading_spinner_mc) && Boolean(this.m_leaderboard_spinner_visible))
         {
            this.m_leaderboard_spinner_visible = false;
            this.leaderboard_loading_spinner_mc.gotoAndPlay("trans_out");
         }
      }
      
      public function update_leaderboard_enabled(param1:Boolean, param2:String) : *
      {
         this.leaderboard_library.update_leaderboard_enabled(param1,param2);
      }
      
      public function register_listeners() : *
      {
         ExternalInterface.call("RegisterInputListener",this.m_layer_name,ButtonCode.A_ALT);
         ExternalInterface.call("RegisterInputListener",this.m_layer_name,ButtonCode.B_ALT);
         ExternalInterface.call("RegisterInputListener",this.m_layer_name,ButtonCode.X_ALT);
         ExternalInterface.call("RegisterInputListener",this.m_layer_name,ButtonCode.LB);
         ExternalInterface.call("RegisterInputListener",this.m_layer_name,ButtonCode.LSTICK_LEFT);
         ExternalInterface.call("RegisterInputListener",this.m_layer_name,ButtonCode.LSTICK_RIGHT);
         ExternalInterface.call("RegisterInputListener",this.m_layer_name,ButtonCode.LSTICK_UP);
         ExternalInterface.call("RegisterInputListener",this.m_layer_name,ButtonCode.LSTICK_DOWN);
         ExternalInterface.call("RegisterInputListener",this.m_layer_name,ButtonCode.BACK);
         ExternalInterface.call("RegisterInputAnalogueListener",this.m_layer_name,ButtonCode.ANALOGUE_LTRIG);
         ExternalInterface.call("RegisterInputAnalogueListener",this.m_layer_name,ButtonCode.ANALOGUE_RTRIG);
         ExternalInterface.call("RegisterInputAnalogueListener",this.m_layer_name,ButtonCode.ANALOGUE_LSTICK_RADIAL);
      }
      
      public function unregister_listeners() : *
      {
         ExternalInterface.call("UnregisterInputListener",this.m_layer_name,ButtonCode.A_ALT);
         ExternalInterface.call("UnregisterInputListener",this.m_layer_name,ButtonCode.B_ALT);
         ExternalInterface.call("UnregisterInputListener",this.m_layer_name,ButtonCode.X_ALT);
         ExternalInterface.call("UnregisterInputListener",this.m_layer_name,ButtonCode.LB);
         ExternalInterface.call("UnregisterInputListener",this.m_layer_name,ButtonCode.LSTICK_LEFT);
         ExternalInterface.call("UnregisterInputListener",this.m_layer_name,ButtonCode.LSTICK_RIGHT);
         ExternalInterface.call("UnregisterInputListener",this.m_layer_name,ButtonCode.LSTICK_UP);
         ExternalInterface.call("UnregisterInputListener",this.m_layer_name,ButtonCode.LSTICK_DOWN);
         ExternalInterface.call("UnregisterInputListener",this.m_layer_name,ButtonCode.BACK);
         ExternalInterface.call("UnregisterInputAnalogueListener",this.m_layer_name,ButtonCode.ANALOGUE_LTRIG);
         ExternalInterface.call("UnregisterInputAnalogueListener",this.m_layer_name,ButtonCode.ANALOGUE_RTRIG);
         ExternalInterface.call("UnregisterInputAnalogueListener",this.m_layer_name,ButtonCode.ANALOGUE_LSTICK_RADIAL);
      }
      
      public function target_leaderboard() : *
      {
         if(!this.m_targetting_leaderboard)
         {
            this.register_listeners();
            this.m_targetting_leaderboard = true;
         }
      }
      
      public function untarget_leaderboard() : *
      {
         if(this.m_targetting_leaderboard)
         {
            this.unregister_listeners();
            this.m_targetting_leaderboard = false;
         }
      }
      
      public function fadeOut() : *
      {
         this.fader.gotoAndPlay("trans_in");
      }
      
      public function fadeIn() : *
      {
         this.fader.gotoAndPlay("trans_out");
      }
      
      // Invoked by every DLC SurvivorUILogic on LevelComplete (FlashInvoke
      // method=challengemapOpenedResults). Retail never defined it. AIMP uses it
      // as the generic local mission-complete edge: outside a frontend playlist
      // the retail results branch (indexed by "Playlist Order No") never runs.
      public function challengemapOpenedResults() : void
      {
         this.aimp_send_launcher_message("MISSION_RESULTS_OPENED|source=challenge_map_overlay|phase=gameplay");
      }
      
      public function AIMP_CathodePing() : void
      {
         this.AIMP_CathodeEvent(0,1,0,0);
      }
      
      public function AIMP_CathodeEvent(param1:int = 0, param2:int = 1, param3:int = 0, param4:int = 0) : void
      {
         var _loc5_:String = "AIMP_CATHODE_EVENT";
         _loc5_ += "|source=challenge_map_overlay";
         _loc5_ += "|phase=gameplay";
         _loc5_ += "|method=AIMP_CathodeEvent";
         _loc5_ += "|eventId=" + String(param1);
         _loc5_ += "|value=" + String(param2);
         _loc5_ += "|arg0=" + String(param1);
         _loc5_ += "|arg1=" + String(param2);
         _loc5_ += "|arg2=" + String(param3);
         _loc5_ += "|arg3=" + String(param4);
         this.aimp_send_launcher_message(_loc5_);
      }
      
      public function AIMP_CathodeInt(param1:int = 0, param2:int = 0, param3:int = 0, param4:int = 0) : void
      {
         var _loc5_:String = "AIMP_CATHODE_EVENT";
         _loc5_ += "|source=challenge_map_overlay";
         _loc5_ += "|phase=gameplay";
         _loc5_ += "|method=AIMP_CathodeInt";
         _loc5_ += "|type=int";
         _loc5_ += "|eventId=" + String(param1);
         _loc5_ += "|value=" + String(param2);
         _loc5_ += "|arg0=" + String(param1);
         _loc5_ += "|arg1=" + String(param2);
         _loc5_ += "|arg2=" + String(param3);
         _loc5_ += "|arg3=" + String(param4);
         this.aimp_send_launcher_message(_loc5_);
      }
      
      public function AIMP_CathodeFloat(param1:Number = 0, param2:Number = 0, param3:Number = 0, param4:Number = 0) : void
      {
         var _loc5_:String = "AIMP_CATHODE_EVENT";
         _loc5_ += "|source=challenge_map_overlay";
         _loc5_ += "|phase=gameplay";
         _loc5_ += "|method=AIMP_CathodeFloat";
         _loc5_ += "|type=float";
         _loc5_ += "|eventId=" + String(int(param1));
         _loc5_ += "|value=" + String(param2);
         _loc5_ += "|arg0=" + String(param1);
         _loc5_ += "|arg1=" + String(param2);
         _loc5_ += "|arg2=" + String(param3);
         _loc5_ += "|arg3=" + String(param4);
         this.aimp_send_launcher_message(_loc5_);
      }
      
      public function AIMP_CathodeVector3(param1:Number = 0, param2:Number = 0, param3:Number = 0, param4:Number = 0) : void
      {
         var _loc5_:String = "AIMP_CATHODE_EVENT";
         _loc5_ += "|source=challenge_map_overlay";
         _loc5_ += "|phase=gameplay";
         _loc5_ += "|method=AIMP_CathodeVector3";
         _loc5_ += "|type=vector3";
         _loc5_ += "|eventId=" + String(int(param4));
         _loc5_ += "|x=" + String(param1);
         _loc5_ += "|y=" + String(param2);
         _loc5_ += "|z=" + String(param3);
         _loc5_ += "|arg0=" + String(param1);
         _loc5_ += "|arg1=" + String(param2);
         _loc5_ += "|arg2=" + String(param3);
         _loc5_ += "|arg3=" + String(param4);
         this.aimp_send_launcher_message(_loc5_);
      }
      
      private function aimp_init_launcher_bridge() : void
      {
         try
         {
            if(this.aimp_launcher_socket != null && this.aimp_launcher_socket.connected)
            {
               return;
            }
            this.aimp_launcher_connected = false;
            this.aimp_launcher_socket = new Socket();
            this.aimp_launcher_socket.addEventListener(Event.CONNECT,this.aimp_launcher_connect_handler,false,0,true);
            this.aimp_launcher_socket.addEventListener(Event.CLOSE,this.aimp_launcher_close_handler,false,0,true);
            this.aimp_launcher_socket.addEventListener(IOErrorEvent.IO_ERROR,this.aimp_launcher_io_error_handler,false,0,true);
            this.aimp_launcher_socket.addEventListener(SecurityErrorEvent.SECURITY_ERROR,this.aimp_launcher_security_error_handler,false,0,true);
            this.aimp_launcher_socket.connect("127.0.0.1",12346);
         }
         catch(error:Error)
         {
            this.aimp_launcher_connected = false;
         }
      }
      
      private function aimp_send_launcher_message(param1:String) : void
      {
         if(param1 == null || param1.length == 0)
         {
            return;
         }
         if(this.aimp_launcher_socket != null && this.aimp_launcher_socket.connected)
         {
            try
            {
               this.aimp_launcher_socket.writeUTFBytes(param1 + "\n");
               this.aimp_launcher_socket.flush();
               return;
            }
            catch(error:Error)
            {
               this.aimp_launcher_connected = false;
            }
         }
         this.aimp_launcher_queue.push(param1);
         this.aimp_init_launcher_bridge();
      }
      
      private function aimp_launcher_connect_handler(param1:Event) : void
      {
         var _loc2_:String = null;
         this.aimp_launcher_connected = true;
         this.aimp_launcher_socket.writeUTFBytes("HELLO|source=challenge_map_overlay|phase=gameplay\n");
         while(this.aimp_launcher_queue.length > 0 && this.aimp_launcher_socket != null && this.aimp_launcher_socket.connected)
         {
            _loc2_ = String(this.aimp_launcher_queue.shift());
            this.aimp_launcher_socket.writeUTFBytes(_loc2_ + "\n");
         }
         this.aimp_launcher_socket.flush();
      }
      
      private function aimp_launcher_close_handler(param1:Event) : void
      {
         this.aimp_launcher_connected = false;
      }
      
      private function aimp_launcher_io_error_handler(param1:IOErrorEvent) : void
      {
         this.aimp_launcher_connected = false;
      }
      
      private function aimp_launcher_security_error_handler(param1:SecurityErrorEvent) : void
      {
         this.aimp_launcher_connected = false;
      }
      
      public function show_network_message(param1:String) : *
      {
         this.connection_popup_mc.connection_sub_mc.txt.text = param1;
         this.connection_popup_mc.gotoAndPlay("show");
      }
      
      public function showSplash() : *
      {
         gotoAndPlay("Intro");
         this.multTotal = 0;
         this.obj1_mc.gotoAndStop("idle");
         this.obj2_mc.gotoAndStop("idle");
         this.obj3_mc.gotoAndStop("idle");
         thefinaltime = 0;
         this.finalScore = 0;
         this.leaderboard_request_timer.addEventListener(TimerEvent.TIMER,this.OnRequestCallback);
      }
      
      public function setCharacter(param1:int) : *
      {
         if(param1 == 1)
         {
            this.titlecard_mc.character_mc.txt.text = "AI_UI_CHAR_NAME_1";
         }
         else if(param1 == 2)
         {
            this.titlecard_mc.character_mc.txt.text = "A1_DLC04_CHAR_NAME_8";
         }
         else if(param1 == 3)
         {
            this.titlecard_mc.character_mc.txt.text = "AI_UI_CHAR_NAME_2";
         }
         else if(param1 == 4)
         {
            this.titlecard_mc.character_mc.txt.text = "AI_UI_CHAR_NAME_3";
         }
         else if(param1 == 5)
         {
            this.titlecard_mc.character_mc.txt.text = "A1_DLC03_CHAR_NAME_7";
         }
         else if(param1 == 6)
         {
            this.titlecard_mc.character_mc.txt.text = "A1_DLC02_CHAR_NAME_6";
         }
         else if(param1 == 7)
         {
            this.titlecard_mc.character_mc.txt.text = "AI_UI_CHAR_NAME_4";
         }
         else if(param1 == 8)
         {
            this.titlecard_mc.character_mc.txt.text = "A1_DLC01_CHAR_NAME_5";
         }
         else if(param1 == 9)
         {
            this.titlecard_mc.character_mc.txt.text = "A1_DLC05_CHAR_NAME_9";
         }
         else
         {
            this.titlecard_mc.character_mc.txt.text = "AI_UI_CHAR_NAME_1";
         }
      }
      
      public function skipIntro() : *
      {
         gotoAndPlay("skipped");
      }
      
      public function dead() : *
      {
         gotoAndPlay("dead");
      }
      
      public function switchOffUI() : *
      {
         gotoAndStop("off");
      }
      
      public function setScoreValues(param1:int, param2:int, param3:int) : *
      {
         this.obj1 = param1;
         this.obj2 = param2;
         this.obj3 = param3;
      }
      
      public function setDecayValues(param1:int, param2:int, param3:int) : *
      {
         this.obj1Decay = param1;
         this.obj2Decay = param2;
         this.obj3Decay = param3;
      }
      
      public function setObjIcons(param1:int, param2:int, param3:int) : *
      {
         this.obj1Icon.gotoAndStop(param1);
         this.obj2Icon.gotoAndStop(param2);
         this.obj3Icon.gotoAndStop(param3);
      }
      
      public function levelID(param1:int, param2:int, param3:int) : *
      {
         this.obj1Complete = false;
         this.obj2Complete = false;
         this.obj3Complete = false;
         if(param3 == 0)
         {
            this.packPrefix = "~AI_UI_";
         }
         else if(param3 == 1)
         {
            this.packPrefix = "~A1_DLC01_";
         }
         else if(param3 == 2)
         {
            this.packPrefix = "~A1_DLC02_";
         }
         else if(param3 == 3)
         {
            this.packPrefix = "~A1_DLC03_";
         }
         else if(param3 == 4)
         {
            this.packPrefix = "~A1_DLC04_";
         }
         else if(param3 == 5)
         {
            this.packPrefix = "~A1_DLC05_";
         }
         this.titlecard_mc.titletext.txt.text = this.packPrefix + "CH_TITLE_" + param1;
         this.obj1Icon.txt.text = this.packPrefix + "CH_OBJECTIVE_" + param2;
         this.obj2Icon.txt.text = this.packPrefix + "CH_OBJECTIVE_" + (param2 + 1);
         this.obj3Icon.txt.text = this.packPrefix + "CH_OBJECTIVE_" + (param2 + 2);
         this.obj1Icon.mult.text = this.obj1;
         this.obj2Icon.mult.text = this.obj2;
         this.obj3Icon.mult.text = this.obj3;
         this.obj1_mc.txt.text = this.packPrefix + "CH_OBJECTIVE_" + param2;
         this.obj2_mc.txt.text = this.packPrefix + "CH_OBJECTIVE_" + (param2 + 1);
         this.obj3_mc.txt.text = this.packPrefix + "CH_OBJECTIVE_" + (param2 + 2);
         this.obj1_mc.icon_mc.no.text = this.obj1;
         this.obj2_mc.icon_mc.no.text = this.obj2;
         this.obj3_mc.icon_mc.no.text = this.obj3;
         this.completedScreen_mc.title.endTitle_txt.text = this.packPrefix + "CH_TITLE_" + param1;
         this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge1.title.text = "~AI_UI_CHALLENGE_EMPTY_HANDED";
         this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge2.title.text = "~AI_UI_CHALLENGE_NEVER_DETECTED";
         this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge3.title.text = "~AI_UI_CHALLENGE_NO_FLAMER";
         this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge4.title.text = "~AI_UI_CHALLENGE_NO_TRACKER";
         this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge5.title.text = "~AI_UI_CHALLENGE_ENGINEER";
         this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge6.title.text = "~AI_UI_CHALLENGE_NO_PREP";
         this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge7.title.text = "~AI_UI_CHALLENGE_NO_HIDING";
         this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge8.title.text = "~AI_UI_CHALLENGE_GATHERER";
         this.completedScreen_mc.completedObj1BG_mc.name_txt.text = this.packPrefix + "CH_OBJECTIVE_" + param2;
         this.completedScreen_mc.completedObj2BG_mc.name_txt.text = this.packPrefix + "CH_OBJECTIVE_" + (param2 + 1);
         this.completedScreen_mc.completedObj3BG_mc.name_txt.text = this.packPrefix + "CH_OBJECTIVE_" + (param2 + 2);
         this.trigger_mc.gotoAndPlay("idle");
         this.trigger_mc.visible = false;
      }
      
      public function ShutdownForReload() : *
      {
         gotoAndPlay("off");
         this.unregister_listeners();
         stage.removeEventListener(KeyboardEvent.KEY_DOWN,this.inputHandler);
         this.m_leaderboard_spinner_visible = false;
         this.leaderboard_loading_spinner_mc.gotoAndPlay("trans_out");
         this.leaderboard_library.remove_leaderboard_instance();
         this.leaderboard_holder_mc.visible = false;
      }
      
      public function awardPopupHide() : *
      {
         awardPopup_mc.gotoAndPlay("trans_out");
      }
      
      public function failMultiplier(param1:int) : *
      {
         if(param1 == 1)
         {
            this.award1Complete = false;
         }
         else if(param1 == 2)
         {
            this.award2Complete = false;
         }
         else if(param1 == 3)
         {
            this.award3Complete = false;
         }
         else if(param1 == 4)
         {
            this.award4Complete = false;
         }
         else if(param1 == 5)
         {
            this.award5Complete = true;
         }
         else if(param1 == 6)
         {
            this.award6Complete = false;
         }
         else if(param1 == 7)
         {
            this.award7Complete = false;
         }
         else if(param1 == 8)
         {
            this.award8Complete = true;
         }
      }
      
      public function obj1Completed() : *
      {
         this.obj1_mc.gotoAndPlay("completed");
         this.obj1Complete = true;
      }
      
      public function obj2Completed() : *
      {
         this.obj2_mc.gotoAndPlay("completed");
         this.obj2Complete = true;
      }
      
      public function obj3Completed() : *
      {
         this.obj3_mc.gotoAndPlay("completed");
         this.obj3Complete = true;
      }
      
      public function obj1Failed() : *
      {
         this.obj1_mc.gotoAndPlay("failed");
         this.obj1_mc.icon_mc.no.text = "AI_UI_CHALLENGE_FAILED";
         this.obj1 = 0;
      }
      
      public function obj2Failed() : *
      {
         this.obj2_mc.gotoAndPlay("failed");
         this.obj2_mc.icon_mc.no.text = "AI_UI_CHALLENGE_FAILED";
         this.obj2 = 0;
      }
      
      public function obj3Failed() : *
      {
         this.obj3_mc.gotoAndPlay("failed");
         this.obj3_mc.icon_mc.no.text = "AI_UI_CHALLENGE_FAILED";
         this.obj3 = 0;
      }
      
      public function showTimer() : *
      {
         gotoAndStop("idle");
         this.timerHeader.message_txt.text = "AI_UI_TIMER_GET_READY";
      }
      
      public function refresh_leaderboard_timer(param1:Number) : *
      {
         if(this.isSalvageMap == false)
         {
            this.time = param1;
            this.trigger_mc.txt.text = StringUtil.millisecondsToTime(this.time * 1000,":",false,false);
            if(!this.obj1Complete && this.obj1 > 0)
            {
               this.obj1 -= this.obj1Decay;
               this.obj1_mc.icon_mc.no.text = this.obj1;
            }
            if(!this.obj2Complete && this.obj2 > 0)
            {
               this.obj2 -= this.obj2Decay;
               this.obj2_mc.icon_mc.no.text = this.obj2;
            }
            if(!this.obj3Complete && this.obj3 > 0)
            {
               this.obj3 -= this.obj3Decay;
               this.obj3_mc.icon_mc.no.text = this.obj3;
            }
            if(this.obj1 <= 0)
            {
               this.obj1Failed();
            }
            if(this.obj2 <= 0)
            {
               this.obj2Failed();
            }
            if(this.obj3 <= 0)
            {
               this.obj3Failed();
            }
         }
         else if(this.isSalvageMap == true)
         {
            this.time = param1;
            this.salvageTimer_mc.timer_mc.txt.text = StringUtil.millisecondsToTime(this.time * 1000,":",false,true);
            if(Boolean(this.salvageMissionActive) && this.salvageCurrentObjScore > 0)
            {
               this.salvageCurrentObjScore -= this.salvageCurrentObjDecay;
            }
            else if(Boolean(this.salvageMissionActive) && this.salvageCurrentObjScore <= 0)
            {
               this.salvageCurrentObjScore = 0;
            }
            if(this.salvageCurrentWave == 1)
            {
               this.waveHistory.icon1.score.txt.text = this.salvageCurrentObjScore;
            }
            else if(this.salvageCurrentWave == 2)
            {
               this.waveHistory.icon2.score.txt.text = this.salvageCurrentObjScore;
            }
            else if(this.salvageCurrentWave == 3)
            {
               this.waveHistory.icon3.score.txt.text = this.salvageCurrentObjScore;
            }
            else if(this.salvageCurrentWave == 4)
            {
               this.waveHistory.icon4.score.txt.text = this.salvageCurrentObjScore;
            }
            else if(this.salvageCurrentWave == 5)
            {
               this.waveHistory.icon5.score.txt.text = this.salvageCurrentObjScore;
            }
            else if(this.salvageCurrentWave == 6)
            {
               this.waveHistory.icon6.score.txt.text = this.salvageCurrentObjScore;
            }
            else if(this.salvageCurrentWave == 7)
            {
               this.waveHistory.icon7.score.txt.text = this.salvageCurrentObjScore;
            }
            else if(this.salvageCurrentWave == 8)
            {
               this.waveHistory.icon8.score.txt.text = this.salvageCurrentObjScore;
            }
            else if(this.salvageCurrentWave == 9)
            {
               this.waveHistory.icon9.score.txt.text = this.salvageCurrentObjScore;
            }
            else if(this.salvageCurrentWave == 10)
            {
               this.waveHistory.icon10.score.txt.text = this.salvageCurrentObjScore;
            }
         }
      }
      
      public function beginTimer() : *
      {
         gotoAndStop("timerBegin");
         this.trigger_mc.visible = true;
         this.trigger_mc.gotoAndPlay("Begin");
         this.timerHeader.message_txt.text = "AI_UI_TIMER_TIME_ELAPSED";
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","Play_Timer_Start");
         }
      }
      
      private function inputHandler(param1:KeyboardEvent) : void
      {
         if(this.keyboard_input == true)
         {
            this.leaderboard_library.inputHandler(param1);
         }
         else
         {
            this.leaderboard_library.inputHandlerNonPC(param1);
            switch(param1.keyCode)
            {
               case ButtonCode.A_ALT:
                  if(ExternalInterface.available)
                  {
                     ExternalInterface.call("continue_requested",0);
                     break;
                  }
            }
         }
      }
      
      public function calculateFinalScore() : *
      {
         if(!this.obj1Complete)
         {
            this.obj1Failed();
            this.completedScreen_mc.completedObj1BG_mc.gotoAndStop("failed");
         }
         if(!this.obj2Complete)
         {
            this.obj2Failed();
            this.completedScreen_mc.completedObj2BG_mc.gotoAndStop("failed");
         }
         if(!this.obj3Complete)
         {
            this.obj3Failed();
            this.completedScreen_mc.completedObj3BG_mc.gotoAndStop("failed");
         }
         this.completedScreen_mc.completedObj1BG_mc.txt.text = this.obj1;
         this.completedScreen_mc.completedObj2BG_mc.txt.text = this.obj2;
         this.completedScreen_mc.completedObj3BG_mc.txt.text = this.obj3;
         this.gauntletProgressPopup_mc.gauntletResults.gObj1Score.text = this.obj1;
         this.gauntletProgressPopup_mc.gauntletResults.gObj2Score.text = this.obj2;
         this.gauntletProgressPopup_mc.gauntletResults.gObj3Score.text = this.obj3;
         if(this.award1Complete)
         {
            this.multTotal += 3;
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge1.txt.text = "+3";
         }
         else
         {
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge1.txt.text = "0";
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge1.gotoAndStop("failed");
         }
         if(this.award2Complete)
         {
            this.multTotal += 3;
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge2.txt.text = "+3";
         }
         else
         {
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge2.txt.text = "0";
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge2.gotoAndStop("failed");
         }
         if(this.award3Complete)
         {
            this.multTotal += 2;
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge3.txt.text = "+2";
         }
         else
         {
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge3.txt.text = "0";
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge3.gotoAndStop("failed");
         }
         if(this.award4Complete)
         {
            this.multTotal += 4;
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge4.txt.text = "+4";
         }
         else
         {
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge4.txt.text = "0";
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge4.gotoAndStop("failed");
         }
         if(this.award5Complete)
         {
            this.miscPoints += 3000;
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge5.txt.text = "3000";
         }
         else
         {
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge5.txt.text = "0";
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge5.gotoAndStop("failed");
         }
         if(this.award6Complete)
         {
            this.miscPoints += 4000;
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge6.txt.text = "4000";
         }
         else
         {
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge6.txt.text = "0";
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge6.gotoAndStop("failed");
         }
         if(this.award7Complete)
         {
            this.miscPoints += 2000;
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge7.txt.text = "2000";
         }
         else
         {
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge7.txt.text = "0";
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge7.gotoAndStop("failed");
         }
         if(this.award8Complete)
         {
            this.miscPoints += 5000;
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge8.txt.text = "5000";
         }
         else
         {
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge8.txt.text = "0";
            this.completedScreen_mc.awardPopup_mc.badgeParent_mc.badge8.gotoAndStop("failed");
         }
         this.completedScreen_mc.awardPopup_mc.gotoAndPlay("trans_in");
         this.completedScreen_mc.awardPopup_mc.badgeParent_mc.totalMult_txt.text = this.multTotal;
         this.gauntletProgressPopup_mc.gauntletResults.gMultipliers.text = this.multTotal;
         this.completedScreen_mc.awardPopup_mc.badgeParent_mc.bonusScore_txt.text = this.miscPoints;
         this.gauntletProgressPopup_mc.gauntletResults.gBonusPoints.text = this.miscPoints;
         if(this.multTotal < 1)
         {
            this.finalScore = int(this.obj1 + this.obj2 + this.obj3 + this.miscPoints);
         }
         else
         {
            this.finalScore = int((this.obj1 + this.obj2 + this.obj3 + this.miscPoints) * this.multTotal);
         }
         this.GenericCallbackInt("finalScore",int(this.finalScore));
         this.completedScreen_mc.finalScore_mc.txt.text = this.finalScore;
         this.gauntletProgressPopup_mc.gauntletResults.gProgressScore.text = this.finalScore;
         this.completedScreen_mc.finalTime_mc.endScore_txt.text = StringUtil.millisecondsToTime(this.time * 1000,":",false,false);
         this.gauntletProgressPopup_mc.gauntletResults.gProgressTime.text = StringUtil.millisecondsToTime(this.time * 1000,":",false,false);
         this.GenericCallbackFloat("finalTime",Number(this.time * 1000));
         this.GenericCallbackInt("scoreReady",int(1));
      }
      
      public function gotoEndScreen() : *
      {
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","Play_Timer_Stop");
         }
         if(ExternalInterface.available)
         {
            ExternalInterface.call("PlaySound","Play_Score_Multiplier");
         }
         gotoAndPlay("ChallengeComplete");
         this.completedScreen_mc.gotoAndPlay("trans_in");
         this.leaderboard_library.set_leaderboard_holder(this.leaderboard_holder_mc);
         this.leaderboard_library.set_leaderboard_level(this.leaderboard_library.get_current_level());
         ExternalInterface.call("RegisterInputListener",this.m_layer_name,ButtonCode.Y_ALT);
         this.target_leaderboard();
         stage.addEventListener(KeyboardEvent.KEY_DOWN,this.inputHandler);
         this.submit_timed_leaderboard_request();
      }
      
      internal function submit_timed_leaderboard_request() : void
      {
         this.leaderboard_request_timer.start();
      }
      
      internal function OnRequestCallback(param1:TimerEvent) : void
      {
         this.leaderboard_request_timer.stop();
         this.leaderboard_request_timer.removeEventListener(TimerEvent.TIMER,this.OnRequestCallback);
         this.leaderboard_library.submit_request();
      }
      
      public function gauntletProgress() : *
      {
         gotoAndStop("GuantletProgress");
         this.gauntletProgressPopup_mc.gotoAndPlay("trans_in");
      }
      
      public function gauntletTransition() : *
      {
         this.gauntletProgressPopup_mc.gotoAndPlay("trans_out");
      }
      
      public function gauntletFinal() : *
      {
         gotoAndStop("GauntletEnd");
         this.playlistEndResults_mc.visible = true;
         this.playlistEndResults_mc.gotoAndPlay("trans_in");
         this.leaderboard_library.set_leaderboard_holder(this.leaderboard_holder_mc);
         this.leaderboard_library.set_leaderboard_level(this.leaderboard_library.get_current_level());
         ExternalInterface.call("RegisterInputListener",this.m_layer_name,ButtonCode.Y_ALT);
         this.target_leaderboard();
         stage.addEventListener(KeyboardEvent.KEY_DOWN,this.inputHandler);
         this.submit_timed_leaderboard_request();
      }
      
      public function gLevelScores(param1:int, param2:int, param3:int) : *
      {
         var _loc4_:* = param1 + param2 + param3;
         this.playlistEndResults_mc.map1_score.txt.text = param1;
         this.playlistEndResults_mc.map2_score.txt.text = param2;
         this.playlistEndResults_mc.map3_score.txt.text = param3;
         this.playlistEndResults_mc.finalScoreEND_mc.txt.text = _loc4_;
         this.GenericCallbackInt("playlistFinalScore",int(_loc4_));
      }
      
      public function playlistLevelTimes(param1:Number, param2:Number, param3:Number) : *
      {
         var _loc4_:Number = param1 + param2 + param3;
         this.playlistEndResults_mc.map1_time.txt.text = StringUtil.millisecondsToTime(param1,":",false,false);
         this.playlistEndResults_mc.map2_time.txt.text = StringUtil.millisecondsToTime(param2,":",false,false);
         this.playlistEndResults_mc.map3_time.txt.text = StringUtil.millisecondsToTime(param3,":",false,false);
         this.playlistEndResults_mc.finalTimeEND_mc.txt.text = StringUtil.millisecondsToTime(_loc4_,":",false,true);
         this.GenericCallbackFloat("playlistFinalTime",Number(_loc4_));
      }
      
      public function updateMarathonTime(param1:Number, param2:Number) : *
      {
         var _loc3_:Number = param2 + param1;
         this.marathonProgressPopup_mc.marathonResults.mProgressTime.text = StringUtil.millisecondsToTime(_loc3_,":",false,true);
         this.GenericCallbackFloat("marathonUpdateTime",Number(_loc3_));
      }
      
      public function updateMarathonScore(param1:int, param2:int) : *
      {
         var _loc3_:Number = param2 + param1;
         this.marathonProgressPopup_mc.marathonResults.mProgressScore.text = _loc3_;
         this.GenericCallbackInt("marathonUpdateScore",int(_loc3_));
      }
      
      public function marathonFinal() : *
      {
         gotoAndStop("MarathonEnd");
         this.marathonProgressPopup_mc.visible = true;
         this.marathonProgressPopup_mc.gotoAndPlay("trans_in");
      }
      
      internal function frame1() : *
      {
         stop();
         this.timerHeader.visible = false;
         this.obj1_mc.visible = false;
         this.obj2_mc.visible = false;
         this.obj3_mc.visible = false;
         this.gauntletProgressPopup_mc.visible = false;
         this.leaderboard_holder_mc.visible = false;
         this.marathonProgressPopup_mc.visible = false;
         this.progressContinue_mc.visible = false;
      }
      
      internal function frame2() : *
      {
         this.titlecard_mc.visible = true;
         this.titlecard_mc.gotoAndPlay("trans_in");
         this.continuePrompt_mc.visible = false;
         this.completedScreen_mc.visible = false;
         this.leaderboard_holder_mc.visible = false;
      }
      
      internal function frame84() : *
      {
         this.titlecard_mc.visible = false;
         this.obj1Icon.visible = true;
         this.obj1Icon.iconMask_mc.gotoAndPlay("trans_in");
         this.playObjSound();
      }
      
      internal function frame123() : *
      {
         this.obj1Icon.visible = false;
         this.obj2Icon.visible = true;
         this.obj2Icon.iconMask_mc.gotoAndPlay("trans_in");
         this.playObjSound();
      }
      
      internal function frame163() : *
      {
         this.obj2Icon.visible = false;
         this.obj3Icon.visible = true;
         this.obj3Icon.iconMask_mc.gotoAndPlay("trans_in");
         this.playObjSound();
      }
      
      internal function frame204() : *
      {
         this.obj3Icon.visible = false;
         this.playAllObjSound();
      }
      
      internal function frame209() : *
      {
         this.obj1Icon.visible = false;
         this.obj2Icon.visible = false;
         this.obj3Icon.visible = false;
         this.obj1_mc.visible = true;
         this.obj2_mc.visible = true;
         this.obj3_mc.visible = true;
         this.timerHeader.visible = true;
         stop();
         this.timerHeader.message_txt.text = "AI_UI_TIMER_GET_READY";
      }
      
      internal function frame210() : *
      {
         this.obj1Icon.visible = false;
         this.obj2Icon.visible = false;
         this.obj3Icon.visible = false;
         this.obj1_mc.visible = true;
         this.obj2_mc.visible = true;
         this.obj3_mc.visible = true;
         this.timerHeader.visible = true;
      }
      
      internal function frame211() : *
      {
         this.trigger_mc.visible = false;
         this.obj1_mc.visible = false;
         this.obj2_mc.visible = false;
         this.obj3_mc.visible = false;
         this.timerHeader.visible = false;
         this.continuePrompt_mc.visible = true;
         this.completedScreen_mc.visible = true;
         this.leaderboard_holder_mc.visible = true;
      }
      
      internal function frame228() : *
      {
         stop();
      }
      
      internal function frame229() : *
      {
         stop();
         this.trigger_mc.visible = false;
         this.obj1_mc.visible = false;
         this.obj2_mc.visible = false;
         this.obj3_mc.visible = false;
         this.timerHeader.visible = false;
         this.gauntletProgressPopup_mc.visible = true;
         this.progressContinue_mc.visible = true;
      }
      
      internal function frame238() : *
      {
         stop();
         this.leaderboard_holder_mc.visible = true;
         this.playlistEndResults_mc.visible = true;
         this.progressContinue_mc.visible = false;
         this.continuePrompt_mc.visible = true;
      }
      
      internal function frame247() : *
      {
         stop();
         this.continuePrompt_mc.visible = false;
         this.completedScreen_mc.visible = false;
         this.leaderboard_holder_mc.visible = false;
         this.progressContinue_mc.visible = false;
         trace("SalvageBegin: Setting continue prompt to hidden ");
      }
      
      internal function frame252() : *
      {
         stop();
      }
      
      internal function frame253() : *
      {
         stop();
         this.continuePrompt_mc.visible = true;
         trace("SalvageEnd: Setting continue prompt to visible ");
      }
      
      internal function frame256() : *
      {
         stop();
         this.progressContinue_mc.visible = true;
      }
      
      internal function frame261() : *
      {
         stop();
      }
      
      internal function frame262() : *
      {
         this.trigger_mc.visible = false;
         this.obj1_mc.visible = false;
         this.obj2_mc.visible = false;
         this.obj3_mc.visible = false;
         this.timerHeader.visible = false;
         this.continuePrompt_mc.visible = false;
         this.completedScreen_mc.visible = false;
         this.leaderboard_holder_mc.visible = false;
         this.waveHistory.visible = false;
         this.salvageTimer_mc.visible = false;
      }
      
      internal function frame268() : *
      {
         stop();
      }
   }
}
