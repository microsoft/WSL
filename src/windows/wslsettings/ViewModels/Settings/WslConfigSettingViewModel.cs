// Copyright (C) Microsoft Corporation. All rights reserved.

using CommunityToolkit.Mvvm.ComponentModel;
using Microsoft.UI.Dispatching;
using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using WslSettings.Contracts.Services;

namespace WslSettings.ViewModels.Settings
{
    abstract public partial class WslConfigSettingViewModel : ObservableRecipient
    {
        private readonly DispatcherQueue _dispatcherQueue = DispatcherQueue.GetForCurrentThread();

        protected WslConfigSettingViewModel()
        {
            App.GetService<IWslConfigService>().WslConfigChanged += OnConfigChanged;
        }

        public void OnConfigChanged()
        {
            InitializeConfigSettings();
            _dispatcherQueue.TryEnqueue(() =>
            {
                OnPropertyChanged(String.Empty);
            });
        }

        abstract protected void InitializeConfigSettings();

        protected bool ValidateInput(string? newValue, Regex regex, [CallerMemberName] string? propertyName = null)
        {
            if (newValue == null || !regex.IsMatch(newValue))
            {
                // Notify the property so it can revert back to its previous value.
                OnPropertyChanged(propertyName);
                return false;
            }

            return true;
        }

        protected void Set<T>(ref IWslConfigSetting wslConfigSetting, T newValue, [CallerMemberName] string? propertyName = null)
        {
            if (wslConfigSetting.Equals(newValue))
            {
                return;
            }

            if (wslConfigSetting.SetValue(newValue) != 0)
            {
                SettingsContentVisibility = false;
                ErrorVisibility = !SettingsContentVisibility;
                return;
            }

            OnPropertyChanged(propertyName);
        }

        private bool _errorVisibility = false;
        public bool ErrorVisibility
        {
            get => _errorVisibility;
            set => SetProperty(ref _errorVisibility, value);
        }

        private bool _settingsContentVisibility = true;
        public bool SettingsContentVisibility
        {
            get => _settingsContentVisibility;
            set => SetProperty(ref _settingsContentVisibility, value);
        }
    }
}
