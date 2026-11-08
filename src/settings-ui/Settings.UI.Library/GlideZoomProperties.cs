// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.Text.Json.Serialization;

namespace Microsoft.PowerToys.Settings.UI.Library
{
    public class GlideZoomProperties
    {
        public GlideZoomProperties()
        {
        }

        [JsonConstructor]
        public GlideZoomProperties(IntProperty zoomScope, IntProperty scopeIndicatorOpacity, IntProperty overviewOpacity)
        {
            // A missing field identifies legacy settings. New configurations use
            // independent monitors; existing configurations retain one view.
            ZoomScope = zoomScope ?? new IntProperty(0);
            ScopeIndicatorOpacity = scopeIndicatorOpacity ?? new IntProperty(55);

            // Earlier settings used one opacity for both indicator styles.
            OverviewOpacity = overviewOpacity ?? new IntProperty(ScopeIndicatorOpacity.Value);
        }

        public IntProperty WheelModifier { get; set; } = new IntProperty(0);

        public IntProperty MaximumZoom { get; set; } = new IntProperty(8);

        public IntProperty Sensitivity { get; set; } = new IntProperty(1);

        public BoolProperty DoubleControl { get; set; } = new BoolProperty(false);

        public BoolProperty ModifiedDoubleClick { get; set; } = new BoolProperty(false);

        public BoolProperty ReverseDirection { get; set; } = new BoolProperty(false);

        public BoolProperty CenterFollow { get; set; } = new BoolProperty(false);

        public BoolProperty ThreeFingerTouchpad { get; set; } = new BoolProperty(false);

        public BoolProperty ZoomScaledPointer { get; set; } = new BoolProperty(true);

        public IntProperty PointerSpeed { get; set; } = new IntProperty(100);

        public IntProperty ZoomMultiplier { get; set; } = new IntProperty(1);

        public IntProperty EdgePanMultiplier { get; set; } = new IntProperty(1);

        public IntProperty ZoomScope { get; set; } = new IntProperty(2);

        public IntProperty ScopeIndicatorMode { get; set; } = new IntProperty(0);

        public IntProperty ScopeIndicatorStyle { get; set; } = new IntProperty(0);

        public IntProperty ScopeIndicatorSize { get; set; } = new IntProperty(20);

        public IntProperty ScopeIndicatorOpacity { get; set; } = new IntProperty(55);

        public IntProperty OverviewOpacity { get; set; } = new IntProperty(55);

        public IntProperty ScopeIndicatorBorder { get; set; } = new IntProperty(2);

        public IntProperty ScopeIndicatorDuration { get; set; } = new IntProperty(1500);

        public BoolProperty ScopeIndicatorDetails { get; set; } = new BoolProperty(true);

        public BoolProperty WheelAcceleration { get; set; } = new BoolProperty(false);

        public IntProperty WheelAccelerationLimit { get; set; } = new IntProperty(3);

        public IntProperty ScopeIndicatorX { get; set; } = new IntProperty(100);

        public IntProperty ScopeIndicatorY { get; set; } = new IntProperty(100);

        public IntProperty ScopeIndicatorFade { get; set; } = new IntProperty(120);
    }
}
