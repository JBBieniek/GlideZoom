// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System;
using System.Runtime.CompilerServices;
using Microsoft.PowerToys.Settings.UI.Library;
using Microsoft.PowerToys.Settings.UI.Library.Interfaces;

namespace Microsoft.PowerToys.Settings.UI.ViewModels
{
    public partial class GlideZoomViewModel : PageViewModelBase
    {
        private readonly SettingsUtils _settingsUtils;
        private readonly ISettingsRepository<GeneralSettings> _generalRepository;
        private readonly GlideZoomSettings _settings;
        private readonly Func<string, int> _sendConfig;

        protected override string ModuleName => GlideZoomSettings.ModuleName;

        public GlideZoomViewModel(SettingsUtils settingsUtils, ISettingsRepository<GeneralSettings> generalRepository, ISettingsRepository<GlideZoomSettings> moduleRepository, Func<string, int> sendConfig)
        {
            ArgumentNullException.ThrowIfNull(settingsUtils);
            ArgumentNullException.ThrowIfNull(generalRepository);
            ArgumentNullException.ThrowIfNull(moduleRepository);
            ArgumentNullException.ThrowIfNull(sendConfig);

            _settingsUtils = settingsUtils;
            _generalRepository = generalRepository;
            _settings = moduleRepository.SettingsConfig;
            _sendConfig = sendConfig;
        }

        public bool IsEnabled
        {
            get => _generalRepository.SettingsConfig.Enabled.GlideZoom;
            set
            {
                var generalSettings = _generalRepository.SettingsConfig;
                if (value != generalSettings.Enabled.GlideZoom)
                {
                    generalSettings.Enabled.GlideZoom = value;
                    _sendConfig(new OutGoingGeneralSettings(generalSettings).ToString());
                    OnPropertyChanged();
                }
            }
        }

        public int WheelModifier
        {
            get => _settings.Properties.WheelModifier.Value;
            set => SetInteger(_settings.Properties.WheelModifier, value, 0, 3);
        }

        public int ZoomScope
        {
            get => _settings.Properties.ZoomScope.Value;
            set => SetInteger(_settings.Properties.ZoomScope, value, 0, 2);
        }

        public int MaximumZoomIndex
        {
            get => _settings.Properties.MaximumZoom.Value switch { 2 => 0, 4 => 1, 16 => 3, _ => 2 };
            set
            {
                if (value >= 0 && value <= 3)
                {
                    SetInteger(_settings.Properties.MaximumZoom, 2 << value, 2, 16);
                }
            }
        }

        public int Sensitivity
        {
            get => _settings.Properties.Sensitivity.Value;
            set => SetInteger(_settings.Properties.Sensitivity, value, 0, 2);
        }

        public bool DoubleControl
        {
            get => _settings.Properties.DoubleControl.Value;
            set => SetBoolean(_settings.Properties.DoubleControl, value);
        }

        public bool ModifiedDoubleClick
        {
            get => _settings.Properties.ModifiedDoubleClick.Value;
            set => SetBoolean(_settings.Properties.ModifiedDoubleClick, value);
        }

        public bool ReverseDirection
        {
            get => _settings.Properties.ReverseDirection.Value;
            set => SetBoolean(_settings.Properties.ReverseDirection, value);
        }

        public bool CenterFollow
        {
            get => _settings.Properties.CenterFollow.Value;
            set
            {
                SetBoolean(_settings.Properties.CenterFollow, value);
                OnPropertyChanged(nameof(IsEdgePanEnabled));
            }
        }

        public bool ThreeFingerTouchpad
        {
            get => _settings.Properties.ThreeFingerTouchpad.Value;
            set => SetBoolean(_settings.Properties.ThreeFingerTouchpad, value);
        }

        public bool ZoomScaledPointer
        {
            get => _settings.Properties.ZoomScaledPointer.Value;
            set
            {
                SetBoolean(_settings.Properties.ZoomScaledPointer, value);
                OnPropertyChanged(nameof(IsEdgePanEnabled));
            }
        }

        public bool IsEdgePanEnabled => ZoomScaledPointer && !CenterFollow;

        public int PointerSpeed
        {
            get => _settings.Properties.PointerSpeed.Value;
            set => SetInteger(_settings.Properties.PointerSpeed, value, 25, 300);
        }

        public int ZoomMultiplier
        {
            get => _settings.Properties.ZoomMultiplier.Value;
            set => SetInteger(_settings.Properties.ZoomMultiplier, value, 1, 100);
        }

        public int EdgePanMultiplier
        {
            get => _settings.Properties.EdgePanMultiplier.Value;
            set => SetInteger(_settings.Properties.EdgePanMultiplier, value, 1, 10);
        }

        public int ScopeIndicatorMode
        {
            get => _settings.Properties.ScopeIndicatorMode.Value;
            set
            {
                SetInteger(_settings.Properties.ScopeIndicatorMode, value, 0, 3);
                OnPropertyChanged(nameof(IsScopeIndicatorEnabled));
                OnPropertyChanged(nameof(IsScopeIndicatorOverview));
                OnPropertyChanged(nameof(IsScopeIndicatorOutline));
            }
        }

        public int ScopeIndicatorStyle
        {
            get => _settings.Properties.ScopeIndicatorStyle.Value;
            set
            {
                SetInteger(_settings.Properties.ScopeIndicatorStyle, value, 0, 2);
                OnPropertyChanged(nameof(IsScopeIndicatorOverview));
                OnPropertyChanged(nameof(IsScopeIndicatorOutline));
            }
        }

        public bool IsScopeIndicatorEnabled => ScopeIndicatorMode != 0;

        public bool IsScopeIndicatorOverview => IsScopeIndicatorEnabled && (ScopeIndicatorStyle is 0 or 2);

        public bool IsScopeIndicatorOutline => IsScopeIndicatorEnabled && (ScopeIndicatorStyle is 1 or 2);

        public int OverviewOpacity
        {
            get => _settings.Properties.OverviewOpacity.Value;
            set => SetInteger(_settings.Properties.OverviewOpacity, value, 0, 100);
        }

        public int ScopeIndicatorSize
        {
            get => _settings.Properties.ScopeIndicatorSize.Value;
            set => SetInteger(_settings.Properties.ScopeIndicatorSize, value, 10, 35);
        }

        public int ScopeIndicatorOpacity
        {
            get => _settings.Properties.ScopeIndicatorOpacity.Value;
            set => SetInteger(_settings.Properties.ScopeIndicatorOpacity, value, 10, 90);
        }

        public int ScopeIndicatorBorder
        {
            get => _settings.Properties.ScopeIndicatorBorder.Value;
            set => SetInteger(_settings.Properties.ScopeIndicatorBorder, value, 1, 8);
        }

        public int ScopeIndicatorDuration
        {
            get => _settings.Properties.ScopeIndicatorDuration.Value;
            set => SetInteger(_settings.Properties.ScopeIndicatorDuration, value, 500, 5000);
        }

        public bool ScopeIndicatorDetails
        {
            get => _settings.Properties.ScopeIndicatorDetails.Value;
            set => SetBoolean(_settings.Properties.ScopeIndicatorDetails, value);
        }

        public bool WheelAcceleration
        {
            get => _settings.Properties.WheelAcceleration.Value;
            set => SetBoolean(_settings.Properties.WheelAcceleration, value);
        }

        public int WheelAccelerationLimit
        {
            get => _settings.Properties.WheelAccelerationLimit.Value;
            set => SetInteger(_settings.Properties.WheelAccelerationLimit, value, 1, 4);
        }

        public int ScopeIndicatorX
        {
            get => _settings.Properties.ScopeIndicatorX.Value;
            set => SetInteger(_settings.Properties.ScopeIndicatorX, value, 0, 100);
        }

        public int ScopeIndicatorY
        {
            get => _settings.Properties.ScopeIndicatorY.Value;
            set => SetInteger(_settings.Properties.ScopeIndicatorY, value, 0, 100);
        }

        public int ScopeIndicatorFade
        {
            get => _settings.Properties.ScopeIndicatorFade.Value;
            set => SetInteger(_settings.Properties.ScopeIndicatorFade, value, 0, 400);
        }

        public void RefreshEnabledState() => OnPropertyChanged(nameof(IsEnabled));

        private void SetInteger(IntProperty property, int value, int minimum, int maximum, [CallerMemberName] string propertyName = null)
        {
            if (value >= minimum && value <= maximum && property.Value != value)
            {
                property.Value = value;
                SaveSettings(propertyName);
            }
        }

        private void SetBoolean(BoolProperty property, bool value, [CallerMemberName] string propertyName = null)
        {
            if (property.Value != value)
            {
                property.Value = value;
                SaveSettings(propertyName);
            }
        }

        private void SaveSettings(string propertyName)
        {
            var json = _settings.ToJsonString();
            _settingsUtils.SaveSettings(json, ModuleName);
            _sendConfig("{\"powertoys\":{\"GlideZoom\":" + json + "}}");
            OnPropertyChanged(propertyName);
        }
    }
}
