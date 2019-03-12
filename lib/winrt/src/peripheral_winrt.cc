#include "peripheral_winrt.h"
#include "winrt_cpp.h"

#include <winrt/Windows.Storage.Streams.h>
using namespace winrt::Windows::Storage::Streams;

using winrt::Windows::Devices::Bluetooth::BluetoothCacheMode;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicsResult;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDescriptorsResult;
using winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceServicesResult;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;

#include <sstream>

PeripheralWinrt::PeripheralWinrt(uint64_t bluetoothAddress,
                                 BluetoothLEAdvertisementType advertismentType, const int rssiValue,
                                 const BluetoothLEAdvertisement& advertisment)
{
    this->bluetoothAddress = bluetoothAddress;
    address = formatBluetoothAddress(bluetoothAddress);
    // Random addresses have the two most-significant bits set of the 48-bit address.
    addressType = (bluetoothAddress >= 211106232532992) ? RANDOM : PUBLIC;
    connectable = advertismentType == BluetoothLEAdvertisementType::ConnectableUndirected ||
        advertismentType == BluetoothLEAdvertisementType::ConnectableDirected;
    Update(rssiValue, advertisment);
}

PeripheralWinrt::~PeripheralWinrt()
{
    if (device.has_value() && connectionToken)
    {
        device->ConnectionStatusChanged(connectionToken);
    }
}

void PeripheralWinrt::Update(const int rssiValue, const BluetoothLEAdvertisement& advertisment)
{
    std::string localName = ws2s(advertisment.LocalName().c_str());
    if (!localName.empty())
    {
        name = localName;
    }

    manufacturerData.clear();
    for (auto& ds : advertisment.DataSections())
    {
        if (ds.DataType() == BluetoothLEAdvertisementDataTypes::TxPowerLevel())
        {
            auto d = ds.Data();
            auto dr = DataReader::FromBuffer(d);
            txPowerLevel = dr.ReadByte();
            if (txPowerLevel >= 128)
                txPowerLevel -= 256;
            dr.Close();
        }
        if (ds.DataType() == BluetoothLEAdvertisementDataTypes::ManufacturerSpecificData())
        {
            auto d = ds.Data();
            auto dr = DataReader::FromBuffer(d);
            manufacturerData.resize(d.Length());
            dr.ReadBytes(manufacturerData);
            dr.Close();
        }
    }

    serviceUuids.clear();
    for (auto& uuid : advertisment.ServiceUuids())
    {
        serviceUuids.push_back(toStr(uuid));
    }

    rssi = rssiValue;
}

void PeripheralWinrt::Disconnect()
{
    cachedServices.clear();
    if (device.has_value() && connectionToken)
    {
        device->ConnectionStatusChanged(connectionToken);
    }
    device = std::nullopt;
}

void PeripheralWinrt::GetServiceFromDevice(
    winrt::guid serviceUuid, std::function<void(std::optional<GattDeviceService>, std::string)> callback)
{
    if (device.has_value())
    {
        device->GetGattServicesForUuidAsync(serviceUuid, BluetoothCacheMode::Cached)
            .Completed([=](IAsyncOperation<GattDeviceServicesResult> result, auto& status) {
                if (status == AsyncStatus::Completed)
                {
                    auto& services = result.GetResults();
                    auto& service = services.Services().First();
                    if (service.HasCurrent())
                    {
                        GattDeviceService& s = service.Current();
                        cachedServices.insert(std::make_pair(serviceUuid, CachedService(s)));
                        callback(s, "");
                    }
                    else
                    {
                        callback(std::nullopt, "GetServiceFromDevice: no service with given id");
                    }
                }
                else
                {
                    std::stringstream ss;
                    ss << "GetServiceFromDevice: failed with status: ";
                    ss << int(status);
                    callback(std::nullopt, ss.str());
                }
            });
    }
    else
    {
        callback(std::nullopt, "GetServiceFromDevice: no device currently connected");
    }
}

void PeripheralWinrt::GetService(winrt::guid serviceUuid,
                                 std::function<void(std::optional<GattDeviceService>, std::string)> callback)
{
    auto it = cachedServices.find(serviceUuid);
    if (it != cachedServices.end())
    {
        callback(it->second.service, "");
    }
    else
    {
        GetServiceFromDevice(serviceUuid, callback);
    }
}

void PeripheralWinrt::GetCharacteristicFromService(
    GattDeviceService service, winrt::guid characteristicUuid,
    std::function<void(std::optional<GattCharacteristic>, std::string)> callback)
{
    service.GetCharacteristicsForUuidAsync(characteristicUuid, BluetoothCacheMode::Cached)
        .Completed([=](IAsyncOperation<GattCharacteristicsResult> result, auto& status) {
            if (status == AsyncStatus::Completed)
            {
                auto& characteristics = result.GetResults();
                auto& characteristic = characteristics.Characteristics().First();
                if (characteristic.HasCurrent())
                {
                    winrt::guid serviceUuid = service.Uuid();
                    CachedService& cachedService = cachedServices[serviceUuid];
                    GattCharacteristic& c = characteristic.Current();
                    cachedService.characterisitics.insert(
                        std::make_pair(c.Uuid(), CachedCharacteristic(c)));
                    callback(c, "");
                }
                else
                {
                    callback(std::nullopt, "GetCharacteristicFromService: no characteristic with given id");
                }
            }
            else
            {
                std::stringstream ss;
                ss << "GetCharacteristicsForUuidAsync: failed with status: ";
                ss << int(status);
                callback(std::nullopt, ss.str());
            }
        });
}

void PeripheralWinrt::GetCharacteristic(
    winrt::guid serviceUuid, winrt::guid characteristicUuid,
    std::function<void(std::optional<GattCharacteristic>, std::string)> callback)
{
    auto it = cachedServices.find(serviceUuid);
    if (it != cachedServices.end())
    {
        auto& cachedService = it->second;
        auto cit = cachedService.characterisitics.find(characteristicUuid);
        if (cit != cachedService.characterisitics.end())
        {
            callback(cit->second.characteristic, "");
        }
        else
        {
            GetCharacteristicFromService(cachedService.service, characteristicUuid, callback);
        }
    }
    else
    {
        GetServiceFromDevice(serviceUuid, [=](std::optional<GattDeviceService> service, std::string error) {
            if (service)
            {
                GetCharacteristicFromService(*service, characteristicUuid, callback);
            }
            else
            {
                callback(std::nullopt, error);
            }
        });
    }
}

void PeripheralWinrt::GetDescriptorFromCharacteristic(
    GattCharacteristic characteristic, winrt::guid descriptorUuid,
    std::function<void(std::optional<GattDescriptor>, std::string)> callback)
{
    characteristic.GetDescriptorsForUuidAsync(descriptorUuid, BluetoothCacheMode::Cached)
        .Completed([=](IAsyncOperation<GattDescriptorsResult> result, auto& status) {
            if (status == AsyncStatus::Completed)
            {
                auto& descriptors = result.GetResults();
                auto& descriptor = descriptors.Descriptors().First();
                if (descriptor.HasCurrent())
                {
                    GattDescriptor d = descriptor.Current();
                    winrt::guid characteristicUuid = characteristic.Uuid();
                    winrt::guid descriptorUuid = d.Uuid();
                    winrt::guid serviceUuid = characteristic.Service().Uuid();
                    CachedService& cachedService = cachedServices[serviceUuid];
                    CachedCharacteristic& c = cachedService.characterisitics[characteristicUuid];
                    c.descriptors.insert(std::make_pair(descriptorUuid, d));
                    callback(d, "");
                }
                else
                {
                    callback(std::nullopt, "GetDescriptorFromCharacteristic: no characteristic with given id");
                }
            }
            else
            {
                std::stringstream ss;
                ss << "GetDescriptorFromCharacteristic: failed with status: ";
                ss << int(status);
                callback(std::nullopt, ss.str());
            }
        });
}

void PeripheralWinrt::GetDescriptor(winrt::guid serviceUuid, winrt::guid characteristicUuid, winrt::guid descriptorUuid,
                                    std::function<void(std::optional<GattDescriptor>, std::string)> callback)
{
    auto it = cachedServices.find(serviceUuid);
    if (it != cachedServices.end())
    {
        auto& cachedService = it->second;
        auto cit = cachedService.characterisitics.find(characteristicUuid);
        if (cit != cachedService.characterisitics.end())
        {
            GetDescriptorFromCharacteristic(cit->second.characteristic, descriptorUuid, callback);
        }
        else
        {
            GetCharacteristicFromService(
                cachedService.service, characteristicUuid,
                [=](std::optional<GattCharacteristic> characteristic, std::string error) {
                    if (characteristic)
                    {
                        GetDescriptorFromCharacteristic(*characteristic, descriptorUuid, callback);
                    }
                    else
                    {
                        callback(std::nullopt, error);
                    }
                });
        }
    }
    else
    {
        GetServiceFromDevice(serviceUuid, [=](std::optional<GattDeviceService> service, std::string error) {
            if (service)
            {
                GetCharacteristicFromService(
                    *service, characteristicUuid,
                    [=](std::optional<GattCharacteristic> characteristic, std::string charError) {
                        if (characteristic)
                        {
                            GetDescriptorFromCharacteristic(*characteristic, descriptorUuid,
                                                            callback);
                        }
                        else
                        {
                            callback(std::nullopt, charError);
                        }
                    });
            }
            else
            {
                callback(std::nullopt, error);
            }
        });
    }
}
