// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using Microsoft.PowerToys.Settings.UI.Helpers;
using Microsoft.PowerToys.Settings.UI.Library;
using Microsoft.PowerToys.Settings.UI.ViewModels;

namespace Microsoft.PowerToys.Settings.UI.Views
{
    public sealed partial class GlideZoomPage : NavigablePage, IRefreshablePage
    {
        private GlideZoomViewModel ViewModel { get; set; }

        public GlideZoomPage()
        {
            var settingsUtils = SettingsUtils.Default;
            ViewModel = new GlideZoomViewModel(settingsUtils, SettingsRepository<GeneralSettings>.GetInstance(settingsUtils), SettingsRepository<GlideZoomSettings>.GetInstance(settingsUtils), ShellPage.SendDefaultIPCMessage);
            DataContext = ViewModel;
            InitializeComponent();
            Loaded += (s, e) => ViewModel.OnPageLoaded();
        }

        public void RefreshEnabledState() => ViewModel.RefreshEnabledState();
    }
}
