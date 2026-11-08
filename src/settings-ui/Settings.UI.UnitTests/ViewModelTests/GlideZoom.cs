// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

using System.Collections.Generic;
using System.IO.Abstractions;
using System.Text.Json;
using ManagedCommon;
using Microsoft.PowerToys.Settings.UI.Library;
using Microsoft.PowerToys.Settings.UI.Library.Helpers;
using Microsoft.PowerToys.Settings.UI.Library.Interfaces;
using Microsoft.PowerToys.Settings.UI.ViewModels;
using Microsoft.VisualStudio.TestTools.UnitTesting;
using Moq;

namespace ViewModelTests
{
    [TestClass]
    public class GlideZoom
    {
        [TestMethod]
        public void DefaultsMatchEngineContractAndStartDisabled()
        {
            var settings = new GlideZoomSettings();
            using var json = JsonDocument.Parse(settings.ToJsonString());
            var root = json.RootElement;
            var properties = root.GetProperty("properties");

            Assert.AreEqual("GlideZoom", root.GetProperty("name").GetString());
            Assert.AreEqual("1.0", root.GetProperty("version").GetString());
            Assert.AreEqual(0, properties.GetProperty("WheelModifier").GetProperty("value").GetInt32());
            Assert.AreEqual(8, properties.GetProperty("MaximumZoom").GetProperty("value").GetInt32());
            Assert.AreEqual(1, properties.GetProperty("Sensitivity").GetProperty("value").GetInt32());
            Assert.AreEqual(2, properties.GetProperty("ZoomScope").GetProperty("value").GetInt32());
            Assert.AreEqual(0, properties.GetProperty("ScopeIndicatorMode").GetProperty("value").GetInt32());
            Assert.AreEqual(0, properties.GetProperty("ScopeIndicatorStyle").GetProperty("value").GetInt32());
            Assert.AreEqual(20, properties.GetProperty("ScopeIndicatorSize").GetProperty("value").GetInt32());
            Assert.AreEqual(55, properties.GetProperty("ScopeIndicatorOpacity").GetProperty("value").GetInt32());
            Assert.AreEqual(2, properties.GetProperty("ScopeIndicatorBorder").GetProperty("value").GetInt32());
            Assert.AreEqual(1500, properties.GetProperty("ScopeIndicatorDuration").GetProperty("value").GetInt32());
            Assert.IsTrue(properties.GetProperty("ScopeIndicatorDetails").GetProperty("value").GetBoolean());
            Assert.IsFalse(properties.GetProperty("WheelAcceleration").GetProperty("value").GetBoolean());
            Assert.AreEqual(3, properties.GetProperty("WheelAccelerationLimit").GetProperty("value").GetInt32());
            Assert.AreEqual(100, properties.GetProperty("ScopeIndicatorX").GetProperty("value").GetInt32());
            Assert.AreEqual(100, properties.GetProperty("ScopeIndicatorY").GetProperty("value").GetInt32());
            Assert.AreEqual(120, properties.GetProperty("ScopeIndicatorFade").GetProperty("value").GetInt32());
            Assert.IsTrue(properties.GetProperty("ZoomScaledPointer").GetProperty("value").GetBoolean());
            Assert.AreEqual(100, properties.GetProperty("PointerSpeed").GetProperty("value").GetInt32());
            Assert.AreEqual(55, properties.GetProperty("OverviewOpacity").GetProperty("value").GetInt32());
            Assert.IsFalse(properties.GetProperty("ThreeFingerTouchpad").GetProperty("value").GetBoolean());
            Assert.IsFalse(new GeneralSettings().Enabled.GlideZoom);
        }

        [TestMethod]
        public void SparseLegacySettingsPreserveEarlierDefaults()
        {
            var settings = JsonSerializer.Deserialize("{\"name\":\"GlideZoom\",\"version\":\"1.0\",\"properties\":{\"CenterFollow\":{\"value\":true}}}", SettingsSerializationContext.Default.GlideZoomSettings);

            Assert.IsTrue(settings.Properties.CenterFollow.Value);
            Assert.AreEqual(8, settings.Properties.MaximumZoom.Value);
            Assert.AreEqual(1, settings.Properties.ZoomMultiplier.Value);
            Assert.IsTrue(settings.Properties.ZoomScaledPointer.Value);
            Assert.AreEqual(0, settings.Properties.ZoomScope.Value);
            Assert.AreEqual(0, settings.Properties.ScopeIndicatorMode.Value);
            Assert.AreEqual(20, settings.Properties.ScopeIndicatorSize.Value);
            Assert.AreEqual(55, settings.Properties.ScopeIndicatorOpacity.Value);
            Assert.AreEqual(1500, settings.Properties.ScopeIndicatorDuration.Value);
            Assert.IsTrue(settings.Properties.ScopeIndicatorDetails.Value);
            Assert.IsFalse(settings.Properties.WheelAcceleration.Value);
            Assert.AreEqual(3, settings.Properties.WheelAccelerationLimit.Value);
            Assert.AreEqual(100, settings.Properties.ScopeIndicatorX.Value);
            Assert.AreEqual(100, settings.Properties.ScopeIndicatorY.Value);
            Assert.AreEqual(120, settings.Properties.ScopeIndicatorFade.Value);
            Assert.AreEqual(100, settings.Properties.PointerSpeed.Value);
            Assert.AreEqual(55, settings.Properties.OverviewOpacity.Value);
        }

        [TestMethod]
        public void ZoomScopePersistsAllChoicesAndUsesModuleEnvelope()
        {
            var messages = new List<string>();
            var settingsUtils = new Mock<SettingsUtils>(new FileSystem(), null);
            using var viewModel = CreateViewModel(settingsUtils, messages);

            viewModel.ZoomScope = 1;
            viewModel.ZoomScope = 0;
            viewModel.ZoomScope = 2;

            Assert.HasCount(3, messages);
            int[] expectedScopes = [1, 0, 2];
            for (var index = 0; index < messages.Count; index++)
            {
                using var json = JsonDocument.Parse(messages[index]);
                var settings = json.RootElement.GetProperty("powertoys").GetProperty("GlideZoom");
                Assert.AreEqual("1.0", settings.GetProperty("version").GetString());
                Assert.AreEqual(expectedScopes[index], settings.GetProperty("properties").GetProperty("ZoomScope").GetProperty("value").GetInt32());
            }

            settingsUtils.Verify(utils => utils.SaveSettings(It.IsAny<string>(), GlideZoomSettings.ModuleName, It.IsAny<string>()), Times.Exactly(3));
        }

        [TestMethod]
        [DataRow(0)]
        [DataRow(1)]
        [DataRow(2)]
        public void ExplicitScopeSurvivesDeserialization(int scope)
        {
            var original = new GlideZoomSettings();
            original.Properties.ZoomScope.Value = scope;

            var settings = JsonSerializer.Deserialize(original.ToJsonString(), SettingsSerializationContext.Default.GlideZoomSettings);

            Assert.AreEqual(scope, settings.Properties.ZoomScope.Value);
        }

        [TestMethod]
        public void SettingsChangePersistsAndSendsMatchingModuleEnvelope()
        {
            var messages = new List<string>();
            var settingsUtils = new Mock<SettingsUtils>(new FileSystem(), null);
            using var viewModel = CreateViewModel(settingsUtils, messages);

            viewModel.MaximumZoomIndex = 3;
            viewModel.ThreeFingerTouchpad = true;

            Assert.HasCount(2, messages);
            using var json = JsonDocument.Parse(messages[1]);
            var properties = json.RootElement.GetProperty("powertoys").GetProperty("GlideZoom").GetProperty("properties");
            Assert.AreEqual(16, properties.GetProperty("MaximumZoom").GetProperty("value").GetInt32());
            Assert.IsTrue(properties.GetProperty("ThreeFingerTouchpad").GetProperty("value").GetBoolean());
            settingsUtils.Verify(utils => utils.SaveSettings(It.IsAny<string>(), GlideZoomSettings.ModuleName, It.IsAny<string>()), Times.Exactly(2));
        }

        [TestMethod]
        public void InvalidOrUnchangedValuesDoNotPersistOrSend()
        {
            var messages = new List<string>();
            var settingsUtils = new Mock<SettingsUtils>(new FileSystem(), null);
            using var viewModel = CreateViewModel(settingsUtils, messages);

            viewModel.ZoomMultiplier = 0;
            viewModel.ZoomMultiplier = 101;
            viewModel.EdgePanMultiplier = 11;
            viewModel.WheelModifier = -1;
            viewModel.Sensitivity = 3;
            viewModel.MaximumZoomIndex = 4;
            viewModel.ZoomScope = -1;
            viewModel.ZoomScope = 3;
            viewModel.ZoomScope = 2;
            viewModel.ScopeIndicatorMode = 4;
            viewModel.ScopeIndicatorStyle = 3;
            viewModel.ScopeIndicatorSize = 9;
            viewModel.ScopeIndicatorSize = 36;
            viewModel.ScopeIndicatorOpacity = 9;
            viewModel.ScopeIndicatorOpacity = 91;
            viewModel.ScopeIndicatorBorder = 0;
            viewModel.ScopeIndicatorBorder = 9;
            viewModel.ScopeIndicatorDuration = 499;
            viewModel.ScopeIndicatorDuration = 5001;
            viewModel.WheelAccelerationLimit = 0;
            viewModel.WheelAccelerationLimit = 5;
            viewModel.ScopeIndicatorX = -1;
            viewModel.ScopeIndicatorX = 101;
            viewModel.ScopeIndicatorY = -1;
            viewModel.ScopeIndicatorY = 101;
            viewModel.ScopeIndicatorFade = -1;
            viewModel.ScopeIndicatorFade = 401;
            viewModel.PointerSpeed = 24;
            viewModel.PointerSpeed = 301;
            viewModel.PointerSpeed = 100;
            viewModel.OverviewOpacity = -1;
            viewModel.OverviewOpacity = 101;
            viewModel.OverviewOpacity = 55;
            viewModel.ZoomMultiplier = 1;
            viewModel.ThreeFingerTouchpad = false;

            Assert.IsEmpty(messages);
            Assert.AreEqual(1, viewModel.ZoomMultiplier);
            Assert.AreEqual(1, viewModel.EdgePanMultiplier);
            Assert.AreEqual(0, viewModel.WheelModifier);
            Assert.AreEqual(1, viewModel.Sensitivity);
            Assert.AreEqual(2, viewModel.MaximumZoomIndex);
            Assert.AreEqual(2, viewModel.ZoomScope);
            settingsUtils.Verify(utils => utils.SaveSettings(It.IsAny<string>(), It.IsAny<string>(), It.IsAny<string>()), Times.Never);
        }

        [TestMethod]
        public void EdgePanRequiresScaledPointerAndFreePointerMode()
        {
            var messages = new List<string>();
            var settingsUtils = new Mock<SettingsUtils>(new FileSystem(), null);
            using var viewModel = CreateViewModel(settingsUtils, messages);

            Assert.IsTrue(viewModel.IsEdgePanEnabled);
            viewModel.CenterFollow = true;
            Assert.IsFalse(viewModel.IsEdgePanEnabled);
            viewModel.CenterFollow = false;
            viewModel.ZoomScaledPointer = false;
            Assert.IsFalse(viewModel.IsEdgePanEnabled);
        }

        [TestMethod]
        [DataRow(0, true)]
        [DataRow(1, false)]
        [DataRow(2, true)]
        public void IndicatorStylesPersistWithoutChangingVisibilityOrPreviewPosition(int style, bool includesOverview)
        {
            var messages = new List<string>();
            var settingsUtils = new Mock<SettingsUtils>(new FileSystem(), null);
            using var viewModel = CreateViewModel(settingsUtils, messages);

            viewModel.ScopeIndicatorMode = 1;
            viewModel.ScopeIndicatorX = 25;
            viewModel.ScopeIndicatorY = 75;
            viewModel.ScopeIndicatorStyle = style;

            Assert.AreEqual(includesOverview, viewModel.IsScopeIndicatorOverview);
            Assert.AreEqual(style != 0, viewModel.IsScopeIndicatorOutline);
            Assert.IsTrue(viewModel.IsScopeIndicatorEnabled);
            using var json = JsonDocument.Parse(messages[^1]);
            var settingsJson = json.RootElement.GetProperty("powertoys").GetProperty("GlideZoom").GetRawText();
            var settings = JsonSerializer.Deserialize(settingsJson, SettingsSerializationContext.Default.GlideZoomSettings);
            Assert.AreEqual(style, settings.Properties.ScopeIndicatorStyle.Value);
            Assert.AreEqual(1, settings.Properties.ScopeIndicatorMode.Value);
            Assert.AreEqual(1500, settings.Properties.ScopeIndicatorDuration.Value);
            Assert.AreEqual(120, settings.Properties.ScopeIndicatorFade.Value);
            Assert.AreEqual(25, settings.Properties.ScopeIndicatorX.Value);
            Assert.AreEqual(75, settings.Properties.ScopeIndicatorY.Value);

            viewModel.ScopeIndicatorMode = 0;
            Assert.IsFalse(viewModel.IsScopeIndicatorEnabled);
            Assert.IsFalse(viewModel.IsScopeIndicatorOverview);
            Assert.IsFalse(viewModel.IsScopeIndicatorOutline);
        }

        [TestMethod]
        [DataRow(10)]
        [DataRow(55)]
        [DataRow(90)]
        public void LegacyPreviewOpacityInheritsEarlierCustomOpacity(int opacity)
        {
            var json = "{\"name\":\"GlideZoom\",\"version\":\"1.0\",\"properties\":{\"ZoomScaledPointer\":{\"value\":false},\"ScopeIndicatorOpacity\":{\"value\":" + opacity + "}}}";
            var settings = JsonSerializer.Deserialize(json, SettingsSerializationContext.Default.GlideZoomSettings);

            Assert.AreEqual(opacity, settings.Properties.OverviewOpacity.Value);
            Assert.AreEqual(opacity, settings.Properties.ScopeIndicatorOpacity.Value);
            Assert.AreEqual(100, settings.Properties.PointerSpeed.Value);
            Assert.IsFalse(settings.Properties.ZoomScaledPointer.Value);
        }

        [TestMethod]
        [DataRow(25, false)]
        [DataRow(25, true)]
        [DataRow(100, false)]
        [DataRow(100, true)]
        [DataRow(300, false)]
        [DataRow(300, true)]
        public void PointerSpeedAndSeparateOpacitiesPersistWithAutomaticAdjustmentOnOrOff(int speed, bool automatic)
        {
            var messages = new List<string>();
            var settingsUtils = new Mock<SettingsUtils>(new FileSystem(), null);
            using var viewModel = CreateViewModel(settingsUtils, messages);
            viewModel.ZoomScaledPointer = automatic;
            viewModel.PointerSpeed = speed;
            viewModel.ScopeIndicatorMode = 1;
            viewModel.ScopeIndicatorStyle = 2;
            viewModel.ScopeIndicatorOpacity = 85;
            viewModel.OverviewOpacity = automatic ? 0 : 100;

            using var json = JsonDocument.Parse(messages[^1]);
            var settingsJson = json.RootElement.GetProperty("powertoys").GetProperty("GlideZoom").GetRawText();
            var settings = JsonSerializer.Deserialize(settingsJson, SettingsSerializationContext.Default.GlideZoomSettings);
            Assert.AreEqual(speed, settings.Properties.PointerSpeed.Value);
            Assert.AreEqual(automatic, settings.Properties.ZoomScaledPointer.Value);
            Assert.AreEqual(85, settings.Properties.ScopeIndicatorOpacity.Value);
            Assert.AreEqual(automatic ? 0 : 100, settings.Properties.OverviewOpacity.Value);
            Assert.IsTrue(viewModel.IsScopeIndicatorOverview && viewModel.IsScopeIndicatorOutline);
        }

        [TestMethod]
        public void IndicatorAndAccelerationChoicesPersistAndRoundTrip()
        {
            var messages = new List<string>();
            var settingsUtils = new Mock<SettingsUtils>(new FileSystem(), null);
            using var viewModel = CreateViewModel(settingsUtils, messages);
            Assert.IsFalse(viewModel.IsScopeIndicatorEnabled);

            viewModel.ScopeIndicatorMode = 1;
            Assert.IsTrue(viewModel.IsScopeIndicatorOverview);
            viewModel.ScopeIndicatorStyle = 1;
            Assert.IsFalse(viewModel.IsScopeIndicatorOverview);
            viewModel.ScopeIndicatorSize = 35;
            viewModel.ScopeIndicatorOpacity = 90;
            viewModel.ScopeIndicatorBorder = 8;
            viewModel.ScopeIndicatorDuration = 5000;
            viewModel.ScopeIndicatorDetails = false;
            viewModel.WheelAcceleration = true;
            viewModel.WheelAccelerationLimit = 4;
            viewModel.ScopeIndicatorX = 0;
            viewModel.ScopeIndicatorY = 50;
            viewModel.ScopeIndicatorFade = 0;

            Assert.HasCount(12, messages);
            using var json = JsonDocument.Parse(messages[^1]);
            var settingsJson = json.RootElement.GetProperty("powertoys").GetProperty("GlideZoom").GetRawText();
            var settings = JsonSerializer.Deserialize(settingsJson, SettingsSerializationContext.Default.GlideZoomSettings);
            Assert.AreEqual(1, settings.Properties.ScopeIndicatorMode.Value);
            Assert.AreEqual(1, settings.Properties.ScopeIndicatorStyle.Value);
            Assert.AreEqual(35, settings.Properties.ScopeIndicatorSize.Value);
            Assert.AreEqual(90, settings.Properties.ScopeIndicatorOpacity.Value);
            Assert.AreEqual(8, settings.Properties.ScopeIndicatorBorder.Value);
            Assert.AreEqual(5000, settings.Properties.ScopeIndicatorDuration.Value);
            Assert.IsFalse(settings.Properties.ScopeIndicatorDetails.Value);
            Assert.IsTrue(settings.Properties.WheelAcceleration.Value);
            Assert.AreEqual(4, settings.Properties.WheelAccelerationLimit.Value);
            Assert.AreEqual(0, settings.Properties.ScopeIndicatorX.Value);
            Assert.AreEqual(50, settings.Properties.ScopeIndicatorY.Value);
            Assert.AreEqual(0, settings.Properties.ScopeIndicatorFade.Value);
            settingsUtils.Verify(utils => utils.SaveSettings(It.IsAny<string>(), GlideZoomSettings.ModuleName, It.IsAny<string>()), Times.Exactly(12));
        }

        [TestMethod]
        public void EnableUsesGeneralSettingsAndModuleHelper()
        {
            var general = new GeneralSettings();
            var messages = new List<string>();
            var settingsUtils = new Mock<SettingsUtils>(new FileSystem(), null);
            using var viewModel = CreateViewModel(settingsUtils, messages, general);

            viewModel.IsEnabled = true;

            Assert.IsTrue(ModuleHelper.GetIsModuleEnabled(general, ModuleType.GlideZoom));
            Assert.AreEqual("GlideZoom", ModuleHelper.GetModuleKey(ModuleType.GlideZoom));
            using var json = JsonDocument.Parse(messages[0]);
            Assert.IsTrue(json.RootElement.GetProperty("general").GetProperty("enabled").GetProperty("GlideZoom").GetBoolean());
            ModuleHelper.SetIsModuleEnabled(general, ModuleType.GlideZoom, false);
            Assert.IsFalse(viewModel.IsEnabled);
        }

        [TestMethod]
        public void EnableAfterRepositoryReloadPreservesOtherModules()
        {
            var messages = new List<string>();
            var settingsUtils = new Mock<SettingsUtils>(new FileSystem(), null);
            var originalSettings = new GeneralSettings();
            originalSettings.Enabled.GlideZoom = true;
            var generalRepository = new Mock<ISettingsRepository<GeneralSettings>>();
            generalRepository.SetupProperty(repository => repository.SettingsConfig, originalSettings);
            var moduleRepository = new Mock<ISettingsRepository<GlideZoomSettings>>();
            moduleRepository.SetupGet(repository => repository.SettingsConfig).Returns(new GlideZoomSettings());
            using var viewModel = new GlideZoomViewModel(settingsUtils.Object, generalRepository.Object, moduleRepository.Object, message =>
            {
                messages.Add(message);
                return 0;
            });

            var reloadedSettings = new GeneralSettings();
            reloadedSettings.Enabled.FancyZones = false;
            generalRepository.Object.SettingsConfig = reloadedSettings;
            viewModel.RefreshEnabledState();

            Assert.IsFalse(viewModel.IsEnabled);
            viewModel.IsEnabled = true;

            Assert.HasCount(1, messages);
            using var json = JsonDocument.Parse(messages[0]);
            var enabled = json.RootElement.GetProperty("general").GetProperty("enabled");
            Assert.IsTrue(enabled.GetProperty("GlideZoom").GetBoolean());
            Assert.IsFalse(enabled.GetProperty("FancyZones").GetBoolean());
            Assert.IsTrue(reloadedSettings.Enabled.GlideZoom);
            Assert.IsTrue(originalSettings.Enabled.FancyZones);
        }

        private static GlideZoomViewModel CreateViewModel(Mock<SettingsUtils> settingsUtils, List<string> messages, GeneralSettings general = null)
        {
            var generalRepository = new Mock<ISettingsRepository<GeneralSettings>>();
            generalRepository.SetupGet(repository => repository.SettingsConfig).Returns(general ?? new GeneralSettings());
            var moduleRepository = new Mock<ISettingsRepository<GlideZoomSettings>>();
            moduleRepository.SetupGet(repository => repository.SettingsConfig).Returns(new GlideZoomSettings());

            return new GlideZoomViewModel(settingsUtils.Object, generalRepository.Object, moduleRepository.Object, message =>
            {
                messages.Add(message);
                return 0;
            });
        }
    }
}
