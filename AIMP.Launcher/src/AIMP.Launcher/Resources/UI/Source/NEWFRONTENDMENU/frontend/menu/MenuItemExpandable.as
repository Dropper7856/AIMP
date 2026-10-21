package frontend.menu
{
   import frontend.events.MenuPageEvent;

   public class MenuItemExpandable extends MenuItem
   {


      public var autoExpand:Boolean = true;

      private var _subPage:MenuPage = null;

      public function MenuItemExpandable(param1:String, param2:Function = null, param3:Array = null)
      {
         super(param1);
         this._subPage = new MenuPage(param1,param3,this);
         if(param2 != null)
         {
            this._subPage.addEventListener(MenuPageEvent.CHANGE,param2,false,0,true);
         }
      }

      public function get page() : MenuPage
      {
         return this._subPage;
      }

      public function get subPage() : MenuPage
      {
         return this._subPage;
      }

      public function set subPage(param1:MenuPage) : void
      {
         if(param1 != null)
         {
            this._subPage = param1;
            _itemData.name = this._subPage.title;
         }
      }

      override public function onPress() : void
      {
         super.onPress();
         if(this.autoExpand && _menu != null)
         {
            _menu.expand(this._subPage);
         }
      }

      override protected function applyTextCaseChanges() : void
      {
         super.applyTextCaseChanges();
         this._subPage.title = doTextCaseChange(this._subPage.title);
      }
   }
}
