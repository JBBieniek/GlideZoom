// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.Text.Json.Serialization;
using ManagedCommon;
using Microsoft.PowerToys.Settings.UI.Library.Interfaces;

namespace Microsoft.PowerToys.Settings.UI.Library
{
    public class GlideZoomSettings : BasePTModuleSettings, ISettingsConfig
    {
        public const string ModuleName = "GlideZoom";
        public const string ModuleVersion = "1.0";

        public GlideZoomSettings()
        {
            Name = ModuleName;
            Version = ModuleVersion;
            Properties = new GlideZoomProperties();
        }

        [JsonPropertyName("properties")]
        public GlideZoomProperties Properties { get; set; }

        public string GetModuleName() => ModuleName;

        public ModuleType GetModuleType() => ModuleType.GlideZoom;

        public bool UpgradeSettingsConfiguration() => false;
    }
}
