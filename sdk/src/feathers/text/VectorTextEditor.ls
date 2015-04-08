package feathers.text
{
    import feathers.text.VectorTextRenderer;
    import loom.Application;
    import loom.ApplicationEvents;    
    import loom.platform.IMEDelegate;
    import loom.platform.LoomKeyboardType;
    import loom2d.animation.Transitions;
    import loom2d.display.TextAlign;
    import loom2d.Loom2D;
    import loom2d.display.Quad;
    import loom2d.display.DisplayObject;
    import loom2d.display.Stage;
    import loom2d.math.Point;
    import loom2d.math.Rectangle;
    import loom2d.events.Event;
    import loom2d.events.EnterFrameEvent;
    import loom2d.animation.DelayedCall;
    import feathers.core.ITextEditor;
    import feathers.events.FeathersEventType;    
    import feathers.text.TextFormatAlign;

    public class VectorTextEditor extends VectorTextRenderer implements ITextEditor
    {
        protected var imeDelegate:IMEDelegate;
        protected var editableText:String;
        protected var _displayAsPassword:Boolean = false;
        protected var _maxChars:int;
        protected var _restrict:String = "";
        protected var _isEditable:Boolean = true;
        protected var _keyboardType:LoomKeyboardType = 0;
        protected var _caretQuad:Quad = new Quad(2, 16, 0x000000);
        protected var _hasIMEFocus:Boolean = false;
        protected var _cursorDelayedCall:DelayedCall;
        
        protected var cursorBlinkDelay = 0.7;

        protected var composition:String = "";
        
        // the stage is scrolled when bringing up IME text entry and the text editor is obscured
        // this is the Y value of the scroll 
        private var _stageTargetY:Number = 0;

        // tracks the current bitmap font editor, this is important as there may be a number
        // of editors on the screen and the user can switch between them by selecting them
        // which can cause timing issues as the IME keyboard opens/closes to deal with selection
        private static var _currentEditor:VectorTextEditor = null;

        public function VectorTextEditor()
        {
            imeDelegate = new IMEDelegate();
            imeDelegate.onInsertText += handleInsert;
            imeDelegate.onDeleteBackward += handleDeleteBackward;
            imeDelegate.onShowComposition += handleShowComposition;
            _caretQuad.addEventListener( Event.ADDED_TO_STAGE, onCursorAddedToStage );
            _caretQuad.addEventListener( Event.REMOVED_FROM_STAGE, onCursorRemovedFromStage );
            _cursorDelayedCall = new DelayedCall( toggleCursorVisibility, cursorBlinkDelay );
            _cursorDelayedCall.repeatCount = 0;
            
            // Listen in on application events as we're interested in keyboard size changes
            // to clear focus if user closes the OS keyboard (this works on iOS and Android)
            Application.event += onAppEvent;
        }
        
        public function dispose():void
        {
            imeDelegate.onInsertText -= handleInsert;
            imeDelegate.onDeleteBackward -= handleDeleteBackward;
            imeDelegate.onShowComposition -= handleShowComposition;
            Application.event -= onAppEvent;
            
            // if we're the current editor, make sure we clear the reference            
            if (_currentEditor == this)
                _currentEditor = null;
        }

        protected function onAppEvent(type:String, payload:String)        
        {
            if (type == ApplicationEvents.KEYBOARD_RESIZE)
            {            
                var stage = Loom2D.stage;
                
                var resize = int(payload);
                
                // if we're closing the IME text entry box, undo the stage shift
                if (resize == 0)
                {
                    // ensure that the stage scroll is reset and clear our frame listener
                    _stageTargetY = 0;
                    Loom2D.juggler.tween(stage, 0.3, { y: _stageTargetY, transition: Transitions.EASE_IN_OUT } );
                    
                    // if we're the current editor AND we have focus, clear it after no change for a certain amount of time
                    if (_currentEditor == this && _hasIMEFocus)
                    {
                        clearFocus();
                        _currentEditor = null;
                    }
                }
                else if (_hasIMEFocus)
                {                        

                    // resize is in device points, so we need to scale by stageHeight
                    var scale = stage.stageHeight / stage.nativeStageHeight;
                    resize = (resize) * scale;

                    // find the bounds of the text edit field, used to test if 
                    // we're obscured or not (and thus need to scroll)
                    var bounds = getBounds(stage);
                    
                    var safeZone = 16;
                    
                    // detect whether we need to scroll
                    if ((stage.stageHeight - bounds.bottom) < (resize + safeZone))
                    {
                        // we need to scroll!  We do this with some animation
                        // so the user's eye can follow what is happening
                        _stageTargetY = (stage.stageHeight - resize - bounds.bottom - safeZone) / scale;
                        Loom2D.juggler.tween(stage, 0.2, { y: _stageTargetY, transition: Transitions.EASE_OUT } );
                    }

                    // and mark us as the current editor
                    _currentEditor = this;

                }
                 
            }            

        }
        
        protected function handleInsert(inText:String, length:int):void
        {
            if(!_isEditable)
                return;

            if(inText == "\n")
            {
                // We only support single line text input for now.
                clearFocus();
            }

            text += inText;
            
            updateInput();
        }
        
        private function updateInput() {
            var tl = localToGlobal(new Point(0, 0));
            var br = localToGlobal(new Point(width, height));
            var rw = stage.nativeStageWidth/stage.stageWidth;
            var rh = stage.nativeStageHeight/stage.stageHeight;
            tl.x *= rw;
            tl.y *= rh;
            br.x *= rw;
            br.y *= rh;
            imeDelegate.setTextInputRect(new Rectangle(tl.x, tl.y, br.x-tl.x, br.y-tl.y));
            _caretQuad.visible = true;
            _cursorDelayedCall.advanceTime(-_cursorDelayedCall.currentTime);
        }

        protected function handleDeleteBackward():void
        {
            if(!_isEditable)
                return;

            if(text.length > 0)
            {
                //check for a UTF8 character to make sure we delete them fully
                //NOTE: This only supports Latin UTF8 characters for now. Others alphabets such as Cyrillic or Mandarin will have unexpected results
                var newTextLen:int = text.length - 1;
                var endCharID:int = text.charCodeAt(newTextLen - 1);
                if((endCharID == 0xC2) || (endCharID == 0xC3))
                {
                    newTextLen--;
                }
                text = text.substr(0, newTextLen);            
            }
            
            updateInput();
        }
        
        protected function handleShowComposition(inText:String, len:int, start:int, length:int):void
        {
            composition = inText && inText.length > 0 ? inText : "";
            invalidate();
            updateInput();
        }

        public function set text(v:String):void
        {
            // For now, don't allow tabs or newlines.
            var _localRestrict = "\t\n\r";

            // Restrict characters.
            for(var i:int=0; i<v.length; i++)
            {
                var curChar = v.charAt(i);
                if(_restrict.indexOf(curChar) == -1 && _localRestrict.indexOf(curChar) == -1)
                    continue;

                // Splice it out.
                var part1 = v.substring(0, i);
                var part2 = v.substr(i+1);
                v = part1 + part2;
                i--;
            }

            // Enforce length.
            if(v.length > maxChars && maxChars > 0)
                v = v.substr(0, maxChars);

            super.text = v;
            imeDelegate.contentText = v;
            invalidate();
            dispatchEvent(new Event(Event.CHANGE));
        }

        protected function processDisplayText(input:String):String
        {
            input += composition;
            
            if(displayAsPassword)
            {
                var s:String = "";
                for(var i=0; i<input.length; i++)
                    s += "*";
                return s;
            }

            return input;
        }

        public function get keyboardType():LoomKeyboardType
        {
            return _keyboardType;
        }
        
        public function set keyboardType( value:LoomKeyboardType ):void
        {
            _keyboardType = value;
        }
        
        public function get displayAsPassword():Boolean
        {
            return _displayAsPassword;
        }

        public function set displayAsPassword(value:Boolean):void
        {
            _displayAsPassword = value;

            invalidate();
        }

        public function get maxChars():int
        {
            return _maxChars;
        }

        public function set maxChars(value:int):void
        {
            _maxChars = value;
        }

        public function get restrict():String
        {
            return _restrict;
        }

        public function set restrict(value:String):void
        {
            _restrict = value;
        }

        public function get isEditable():Boolean
        {
            return _isEditable;
        }

        public function set isEditable(value:Boolean):void
        {
            _isEditable = value;
        }

        public function get setTouchFocusOnEndedPhase():Boolean
        {
            return true;
        }

        public function setFocus():void
        {
            if(!_hasIMEFocus)
            {
                //trace("Attaching IME");
                updateInput();
                imeDelegate.attachWithIME( _keyboardType );
                addChild(_caretQuad);
                _hasIMEFocus = true;
                dispatchEventWith(FeathersEventType.FOCUS_IN);
            }
        }

        public function clearFocus():void
        {
            if(_hasIMEFocus)
            {
                //trace("Detaching IME");
                imeDelegate.detachWithIME();
                _hasIMEFocus = false;
                removeChild( _caretQuad, false );
                dispatchEventWith(FeathersEventType.FOCUS_OUT);
           }
        }

        public function selectRange(startIndex:int, endIndex:int):void
        {
        }

        public function validate():void
        {
            _shape.setClipRect(0, 0, width, height);
            super.validate();
            
            var tmp:String = processDisplayText(_text);
            var advance:Number = g.textLineAdvance(_textFormat, 0, 0, tmp);
            //trace("advance", advance);
            //var bounds:Rectangle = g.textBoxBounds(_textFormat, 0, 0, isNaN(this.explicitWidth) ? Number.MAX_VALUE : this.explicitWidth, tmp);
            
            // Just show a centered caret if we have no bounds.
            if(advance >= 0)
            {
                _caretQuad.x = advance;
            }
            else
            {
                // Position based on alignment.
                switch(_textFormat.align)
                {
                    case TextAlign.LEFT:
                        _caretQuad.x = 3;
                        break;
                    case TextAlign.CENTER:
                        _caretQuad.x = explicitWidth / 2;
                        break;
                    case TextAlign.RIGHT:
                        _caretQuad.x = explicitWidth - 3;
                        break;
                }
            }

            var caretHeight:Number = _textFormat.size != NaN ? _textFormat.size : _textFormat.lineHeight; 
            _caretQuad.width = 2;
            _caretQuad.height = caretHeight;
            _caretQuad.y = (height - caretHeight) / 2;
            _caretQuad.color = _textFormat.color;
        }
        
        private function onCursorAddedToStage( e:Event ):void
        {
            Loom2D.juggler.add( _cursorDelayedCall );
        }
        
        private function onCursorRemovedFromStage( e:Event ):void
        {
            Loom2D.juggler.remove( _cursorDelayedCall );
        }
        
        private function toggleCursorVisibility():void
        {
            _caretQuad.visible = !_caretQuad.visible;
        }
        
    }
}