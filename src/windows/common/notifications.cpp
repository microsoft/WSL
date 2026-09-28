/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    notifications.cpp

Abstract:

    This file contains notification function definitions.

--*/

#include "precomp.h"
#include <windows.ui.notifications.h>
#include <NotificationActivationCallback.h>
#include <appmodel.h>
#include <wrl\wrappers\corewrappers.h>

using namespace ABI::Windows::Data::Xml::Dom;
using namespace ABI::Windows::UI::Notifications;
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;

using wsl::shared::Localization;

namespace {
HRESULT CreateToastNotifier(IToastNotifier** notifier)
{
    ComPtr<IToastNotificationManagerStatics> toastStatics;
    RETURN_IF_FAILED(Windows::Foundation::GetActivationFactory(
        HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(), &toastStatics));

    return toastStatics->CreateToastNotifierWithId(HStringReference(L"Microsoft.WSL").Get(), notifier);
}

HRESULT CreateXmlDocumentFromString(const wchar_t* xmlString, IXmlDocument** doc)
{
    ComPtr<IXmlDocument> answer;
    RETURN_IF_FAILED(Windows::Foundation::ActivateInstance(HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(), &answer));

    ComPtr<IXmlDocumentIO> docIO;
    RETURN_IF_FAILED(answer.As(&docIO));

    RETURN_IF_FAILED(docIO->LoadXml(HStringReference(xmlString).Get()));

    return answer.CopyTo(doc);
}

HRESULT CreateToastNotification(IXmlDocument* content, IToastNotification** notification)
{
    ComPtr<IToastNotificationFactory> factory;
    RETURN_IF_FAILED(Windows::Foundation::GetActivationFactory(
        HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(), &factory));

    return factory->CreateToastNotification(content, notification);
}

HRESULT DisplayNotification(IXmlDocument* doc)
{
    // Create the notifier
    // Classic Win32 apps MUST use the compat method to create the notifier
    wil::com_ptr<IToastNotifier> notifier;
    RETURN_IF_FAILED(CreateToastNotifier(&notifier));

    // Create the notification itself (using helper method from compat library)
    wil::com_ptr<IToastNotification> toast;
    RETURN_IF_FAILED(CreateToastNotification(doc, &toast));

    // And show it!
    return notifier->Show(toast.get());
}
} // namespace

namespace wsl::windows::common::notifications {
HRESULT DisplayUpdateNotification(const std::wstring& versionString)
try
{
    const std::wstring creationString = std::format(
        LR"(<toast>
                <visual>
                    <binding template='ToastGeneric'>
                        <text>{}</text>
                        <text>{}</text>
                    </binding>
                </visual>
                <actions>
                    <action arguments='{}' content='{}'/>
                    <action arguments='{}' content='{}'/>
                </actions>
            </toast>)",
        Localization::MessageNewWslVersionAvailable(Localization::Options::DontImpersonate),
        Localization::MessageUpdateToVersion(versionString.c_str(), Localization::Options::DontImpersonate),
        wslhost::update_arg,
        Localization::MessageUpdate(Localization::Options::DontImpersonate),
        wslhost::release_notes_arg,
        Localization::MessageViewReleaseNotes(Localization::Options::DontImpersonate));

    wil::com_ptr<IXmlDocument> doc{};
    THROW_IF_FAILED(CreateXmlDocumentFromString(creationString.c_str(), &doc));

    THROW_IF_FAILED(DisplayNotification(doc.get()));

    return S_OK;
}
CATCH_RETURN()

HRESULT DisplayFilesystemNotification(_In_ LPCSTR binaryName)
try
{
    const std::wstring creationString = std::format(
        LR"(<toast>
                <visual>
                    <binding template='ToastGeneric'>
                        <text>{}</text>
                        <text>{}</text>
                    </binding>
                </visual>
                <actions>
                    <action arguments='{} {}' content='{}'/>
                    <action arguments='{} {}' content="{}"/>
                </actions>
            </toast>)",
        Localization::MessagePerformanceTip(Localization::Options::DontImpersonate),
        Localization::MessageProblematicDrvFsUsage(binaryName, Localization::Options::DontImpersonate),
        wslhost::docs_arg,
        wslhost::docs_arg_filesystem_url,
        Localization::MessageViewDocs(Localization::Options::DontImpersonate),
        wslhost::disable_notification_arg,
        LXSS_NOTIFICATION_DRVFS_PERF_DISABLED,
        Localization::MessageDontShowAgain(Localization::Options::DontImpersonate));

    wil::com_ptr<IXmlDocument> doc{};
    THROW_IF_FAILED(CreateXmlDocumentFromString(creationString.c_str(), &doc));

    THROW_IF_FAILED(DisplayNotification(doc.get()));

    return S_OK;
}
CATCH_RETURN()

void DisplayWarningsNotification()
try
{
    const std::wstring creationString = std::format(
        LR"(<toast>
               <visual>
                   <binding template='ToastGeneric'>
                       <text>{}</text>
                   </binding>
               </visual>
               <actions>
                   <action arguments='{}' content='{}'/>
               </actions>
           </toast>)",
        Localization::MessageWarningDuringStartup(),
        wslhost::event_viewer_arg,
        Localization::MessageOpenEventViewer());

    wil::com_ptr<IXmlDocument> doc{};
    THROW_IF_FAILED(CreateXmlDocumentFromString(creationString.c_str(), &doc));

    THROW_IF_FAILED(DisplayNotification(doc.get()));
}
CATCH_LOG()

void DisplayOptionalComponentsNotification()
try
{
    const std::wstring creationString = std::format(
        LR"(<toast>
               <visual>
                   <binding template='ToastGeneric'>
                       <text>{}</text>
                   </binding>
               </visual>
               <actions>
                   <action arguments='{}' content='{}'/>
               </actions>
           </toast>)",
        Localization::MessageMissingOptionalComponents(),
        wslhost::install_prerequisites_arg,
        Localization::MessageInstallMissingOptionalComponents());

    wil::com_ptr<IXmlDocument> doc{};
    THROW_IF_FAILED(CreateXmlDocumentFromString(creationString.c_str(), &doc));

    THROW_IF_FAILED(DisplayNotification(doc.get()));
}
CATCH_LOG()

void DisplayProxyChangeNotification(const std::wstring& message)
try
{
    const std::wstring creationString =
        std::format(LR"(<toast><visual><binding template='ToastGeneric'><text>{}</text></binding></visual></toast>)", message);

    wil::com_ptr<IXmlDocument> doc{};
    THROW_IF_FAILED(CreateXmlDocumentFromString(creationString.c_str(), &doc));

    THROW_IF_FAILED(DisplayNotification(doc.get()));
}
CATCH_LOG()
} // namespace wsl::windows::common::notifications
