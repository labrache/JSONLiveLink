// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

#include "JSONLiveLinkSource.h"

#include "ILiveLinkClient.h"
#include "LiveLinkTypes.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"

#include "Async/Async.h"
#include "Common/UdpSocketBuilder.h"
#include "HAL/RunnableThread.h"
#include "Json.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

#define LOCTEXT_NAMESPACE "JSONLiveLinkSource"

#define RECV_BUFFER_SIZE 1024 * 1024

FJSONLiveLinkSource::FJSONLiveLinkSource(FIPv4Endpoint InEndpoint)
: Socket(nullptr)
, Stopping(false)
, Thread(nullptr)
, WaitTime(FTimespan::FromMilliseconds(100))
{
	// defaults
	DeviceEndpoint = InEndpoint;

	SourceStatus = LOCTEXT("SourceStatus_DeviceNotFound", "Device Not Found");
	SourceType = LOCTEXT("JSONLiveLinkSourceType", "JSON LiveLink");
	SourceMachineName = LOCTEXT("JSONLiveLinkSourceMachineName", "localhost");

	//setup socket
	if (DeviceEndpoint.Address.IsMulticastAddress())
	{
		Socket = FUdpSocketBuilder(TEXT("JSONSOCKET"))
			.AsNonBlocking()
			.AsReusable()
			.BoundToPort(DeviceEndpoint.Port)
			.WithReceiveBufferSize(RECV_BUFFER_SIZE)

			.BoundToAddress(FIPv4Address::Any)
			.JoinedToGroup(DeviceEndpoint.Address)
			.WithMulticastLoopback()
			.WithMulticastTtl(2);
					
	}
	else
	{
		Socket = FUdpSocketBuilder(TEXT("JSONSOCKET"))
			.AsNonBlocking()
			.AsReusable()
			.BoundToAddress(DeviceEndpoint.Address)
			.BoundToPort(DeviceEndpoint.Port)
			.WithReceiveBufferSize(RECV_BUFFER_SIZE);
	}

	RecvBuffer.SetNumUninitialized(RECV_BUFFER_SIZE);

	if ((Socket != nullptr) && (Socket->GetSocketType() == SOCKTYPE_Datagram))
	{
		SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);

		Start();

		SourceStatus = LOCTEXT("SourceStatus_Receiving", "Receiving");
	}
}

FJSONLiveLinkSource::~FJSONLiveLinkSource()
{
	Stop();
	if (Thread != nullptr)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
	if (Socket != nullptr)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
	}
}

void FJSONLiveLinkSource::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
	Client = InClient;
	SourceGuid = InSourceGuid;
}


bool FJSONLiveLinkSource::IsSourceStillValid() const
{
	// Source is valid if we have a valid thread and socket
	bool bIsSourceValid = !Stopping && Thread != nullptr && Socket != nullptr;
	return bIsSourceValid;
}


bool FJSONLiveLinkSource::RequestSourceShutdown()
{
	Stop();

	return true;
}
// FRunnable interface

void FJSONLiveLinkSource::Start()
{
	ThreadName = "JSON UDP Receiver ";
	ThreadName.AppendInt(FAsyncThreadIndex::GetNext());
	
	Thread = FRunnableThread::Create(this, *ThreadName, 128 * 1024, TPri_AboveNormal, FPlatformAffinity::GetPoolThreadMask());
}

void FJSONLiveLinkSource::Stop()
{
	Stopping = true;
}

uint32 FJSONLiveLinkSource::Run()
{
	TSharedRef<FInternetAddr> Sender = SocketSubsystem->CreateInternetAddr();
	
	while (!Stopping)
	{
		if (Socket->Wait(ESocketWaitConditions::WaitForRead, WaitTime))
		{
			uint32 Size;

			while (Socket->HasPendingData(Size))
			{
				int32 Read = 0;

				if (Socket->RecvFrom(RecvBuffer.GetData(), RecvBuffer.Num(), Read, *Sender))
				{
					if (Read > 0)
					{
						TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe> ReceivedData = MakeShareable(new TArray<uint8>());
						ReceivedData->SetNumUninitialized(Read);
						memcpy(ReceivedData->GetData(), RecvBuffer.GetData(), Read);
						AsyncTask(ENamedThreads::GameThread, [this, ReceivedData]() { HandleReceivedData(ReceivedData); });
					}
				}
			}
		}
	}
	return 0;
}

void FJSONLiveLinkSource::HandleReceivedData(TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe> ReceivedData)
{
	FString JsonString;
	JsonString.Empty(ReceivedData->Num());
	for (uint8& Byte : *ReceivedData.Get())
	{
		JsonString += TCHAR(Byte);
	}
	
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		for (TPair<FString, TSharedPtr<FJsonValue>>& JsonField : JsonObject->Values)
		{
			FName SubjectName(*JsonField.Key);
			const TArray<TSharedPtr<FJsonValue>>& BoneArray = JsonField.Value->AsArray();

			bool bCreateSubject = !EncounteredSubjects.Contains(SubjectName);
			if (bCreateSubject)
			{
				FLiveLinkStaticDataStruct StaticDataStruct = FLiveLinkStaticDataStruct(FLiveLinkSkeletonStaticData::StaticStruct());
				FLiveLinkSkeletonStaticData& StaticData = *StaticDataStruct.Cast<FLiveLinkSkeletonStaticData>();

				StaticData.BoneNames.SetNumUninitialized(BoneArray.Num());
				StaticData.BoneParents.SetNumUninitialized(BoneArray.Num());

				for (int BoneIdx=0; BoneIdx < BoneArray.Num(); ++BoneIdx)
				{
					const TSharedPtr<FJsonValue>& Bone = BoneArray[BoneIdx];
					const TSharedPtr<FJsonObject> BoneObject = Bone->AsObject();

					FString BoneName;
					if (BoneObject->TryGetStringField(TEXT("Name"), BoneName))
					{
						StaticData.BoneNames[BoneIdx] = FName(*BoneName);
					}
					else
					{
						// Invalid Json Format
						return;
					}

					int32 BoneParentIdx;
					if (BoneObject->TryGetNumberField("Parent", BoneParentIdx))
					{
						StaticData.BoneParents[BoneIdx] = BoneParentIdx;
					}
					else
					{
						// Invalid Json Format
						return;
					}
				}

				Client->PushSubjectStaticData_AnyThread({SourceGuid, SubjectName}, ULiveLinkAnimationRole::StaticClass(), MoveTemp(StaticDataStruct));
				EncounteredSubjects.Add(SubjectName);
			}

			FLiveLinkFrameDataStruct FrameDataStruct = FLiveLinkFrameDataStruct(FLiveLinkAnimationFrameData::StaticStruct());
			FLiveLinkAnimationFrameData& FrameData = *FrameDataStruct.Cast<FLiveLinkAnimationFrameData>();

			FrameData.Transforms.SetNumUninitialized(BoneArray.Num());
			for (int BoneIdx = 0; BoneIdx < BoneArray.Num(); ++BoneIdx)
			{
				const TSharedPtr<FJsonValue>& Bone = BoneArray[BoneIdx];
				const TSharedPtr<FJsonObject> BoneObject = Bone->AsObject();

				const TArray<TSharedPtr<FJsonValue>>* LocationArray;
				FVector BoneLocation;
				if (BoneObject->TryGetArrayField(TEXT("Location"), LocationArray) 
					&& LocationArray->Num() == 3) // X, Y, Z
				{
					double X = (*LocationArray)[0]->AsNumber();
					double Y = (*LocationArray)[1]->AsNumber();
					double Z = (*LocationArray)[2]->AsNumber();
					BoneLocation = FVector(X, Y, Z);
				}
				else
				{
					// Invalid Json Format
					return;
				}

				const TArray<TSharedPtr<FJsonValue>>* RotationArray;
				FQuat BoneQuat;
				FRotator BoneRotator;
				bool bRotationIsQuat = true;
				if (BoneObject->TryGetArrayField(TEXT("Rotation"), RotationArray))
				{
					if (RotationArray->Num() == 4) {  // X, Y, Z, W
						double X = (*RotationArray)[0]->AsNumber();
						double Y = (*RotationArray)[1]->AsNumber();
						double Z = (*RotationArray)[2]->AsNumber();
						double W = (*RotationArray)[3]->AsNumber();
						BoneQuat = FQuat(X, Y, Z, W);
					}
					else if (RotationArray->Num() == 3) {
						bRotationIsQuat = false;
						double Pitch = (*RotationArray)[0]->AsNumber();
						double Yaw = (*RotationArray)[1]->AsNumber();
						double Roll = (*RotationArray)[2]->AsNumber();
						BoneRotator = FRotator(Pitch, Yaw, Roll);
					}
					else {
						return;
					}
				}
				else
				{
					// Invalid Json Format
					return;
				}

				const TArray<TSharedPtr<FJsonValue>>* ScaleArray;
				FVector BoneScale;
				if (BoneObject->TryGetArrayField(TEXT("Scale"), ScaleArray)
					&& ScaleArray->Num() == 3) // X, Y, Z
				{
					double X = (*ScaleArray)[0]->AsNumber();
					double Y = (*ScaleArray)[1]->AsNumber();
					double Z = (*ScaleArray)[2]->AsNumber();
					BoneScale = FVector(X, Y, Z);
				}
				else
				{
					// Invalid Json Format
					return;
				}
				if (bRotationIsQuat) {
					FrameData.Transforms[BoneIdx] = FTransform(BoneQuat, BoneLocation, BoneScale);
				}
				else {
					FrameData.Transforms[BoneIdx] = FTransform(BoneRotator, BoneLocation, BoneScale);
				}
			}

			Client->PushSubjectFrameData_AnyThread({SourceGuid, SubjectName}, MoveTemp(FrameDataStruct));
		}
	}
}

#undef LOCTEXT_NAMESPACE
