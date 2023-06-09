// Fill out your copyright notice in the Description page of Project Settings.
#include "StableDiffusionSubsystem.h"
#include "IAssetViewport.h"
#include "Engine/GameEngine.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "UObject/SavePackage.h"
#include "LevelEditor.h"
#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"
#include "LevelEditorSubsystem.h"
#include "StableDiffusionImageResult.h"
#include "StableDiffusionToolsSettings.h"
#include "AssetRegistryModule.h"
#include "Components/SceneCaptureComponent2D.h"
#include "ImageUtils.h"
#include "MoviePipelineImageQuantization.h"
#include "ImagePixelData.h"
#include "EngineUtils.h"
#include "SLevelViewport.h"
#include "Dialogs/Dialogs.h"
#include "Engine/TextureRenderTarget2D.h"
#include "DesktopPlatformModule.h"
#include "LayerProcessors/FinalColorLayerProcessor.h"

#define LOCTEXT_NAMESPACE "StableDiffusionSubsystem"

FString UStableDiffusionSubsystem::NormalMaterialAsset = TEXT("/StableDiffusionTools/Materials/M_Normals.M_Normals");


bool FCapturedFramePayload::OnFrameReady_RenderThread(FColor* ColorBuffer, FIntPoint BufferSize, FIntPoint TargetSize) const
{
	OnFrameCapture.Broadcast(ColorBuffer, BufferSize, TargetSize);
	return true;
}

UStableDiffusionSubsystem::UStableDiffusionSubsystem(const FObjectInitializer& initializer)
{
	// Wait for Python to load our derived classes before we construct the bridge
	IPythonScriptPlugin& PythonModule = FModuleManager::LoadModuleChecked<IPythonScriptPlugin>(TEXT("PythonScriptPlugin"));
	PythonModule.OnPythonInitialized().AddLambda([this]() {
		// Make sure that we have our Python derived bridges available in the settings first
		GetMutableDefault<UStableDiffusionToolsSettings>()->ReloadConfig(UStableDiffusionToolsSettings::StaticClass());
		auto BridgeClass = GetDefault<UStableDiffusionToolsSettings>()->GetGeneratorType();
		this->CreateBridge(BridgeClass);

		// Set Python loaded flags and events
		PythonLoaded = true;
		OnPythonLoadedEx.Broadcast();
		OnPythonLoaded.Broadcast();
	}); 
	//PythonModule.OnPythonInitialized().AddUFunction(this, GET_FUNCTION_NAME_CHECKED(UStableDiffusionSubsystem, CreateBridge));
}

bool UStableDiffusionSubsystem::IsBridgeLoaded()
{
	return (GeneratorBridge == nullptr) ? false : true;
}

void UStableDiffusionSubsystem::CreateBridge(TSubclassOf<UStableDiffusionBridge> BridgeClass)
{
	auto BaseClass = UStableDiffusionBridge::StaticClass();
	if (BridgeClass == BaseClass) {
		UE_LOG(LogTemp, Warning, TEXT("Can not create Stable Diffusion Bridge. Only classes deriving from %s can be created."), *BridgeClass->GetName())
			return;
	}

	TArray<UClass*> PythonBridgeClasses;
	GetDerivedClasses(UStableDiffusionBridge::StaticClass(), PythonBridgeClasses);

	for (auto DerivedBridgeClass : PythonBridgeClasses) {
		if (DerivedBridgeClass->IsChildOf(BridgeClass)) {
				
			// We need to create the bridge class from inside Python so that python created objects don't get GC'd
			FPythonCommandEx PythonCommand;
			PythonCommand.Command = FString::Printf(
				TEXT("from bridges import %s; "\
				"bridge = %s.%s(); "\
				"subsystem = unreal.get_editor_subsystem(unreal.StableDiffusionSubsystem); "\
				"subsystem.set_editor_property('GeneratorBridge', bridge)"), 
				*DerivedBridgeClass->GetName(),
				*DerivedBridgeClass->GetName(),
				*DerivedBridgeClass->GetName()
			);
			PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteStatement;
			PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Public;
			bool Result = IPythonScriptPlugin::Get()->ExecPythonCommandEx(PythonCommand);
			if (!Result) {
				UE_LOG(LogTemp, Error, TEXT("Failed to load SD bridge %s"), *DerivedBridgeClass->GetName())
			}

			break;
		}
	}

	//GeneratorBridge = NewObject<UStableDiffusionBridge>(this, FName(*BridgeClass->GetName()), RF_Public | RF_Standalone, BridgeClass->ClassDefaultObject);
	if (GeneratorBridge) {
		//GeneratorBridge->AddToRoot();
		OnBridgeLoadedEx.Broadcast(GeneratorBridge);
	}
	
}

bool UStableDiffusionSubsystem::DependenciesAreInstalled()
{
	return (DependencyManager) ? DependencyManager->AllDependenciesInstalled() : false;
}

void UStableDiffusionSubsystem::InstallDependency(FDependencyManifestEntry Dependency, bool ForceReinstall)
{
	AsyncTask(ENamedThreads::AnyBackgroundHiPriTask, [this, Dependency, ForceReinstall]() {
		if (this->DependencyManager) {
			FDependencyStatus result = this->DependencyManager->InstallDependency(Dependency, ForceReinstall);

			AsyncTask(ENamedThreads::GameThread, [this, result]() {
				this->DependencyManager->OnDependencyInstalled.Broadcast(result);
			});
		}
	});
}

bool UStableDiffusionSubsystem::HasToken()
{
	if (GeneratorBridge) {
		return !GeneratorBridge->GetToken().IsEmpty();
	}
	return false;
}

FString UStableDiffusionSubsystem::GetToken()
{
	if (GeneratorBridge) {
		return GeneratorBridge->GetToken();
	}
	return "";
}

bool UStableDiffusionSubsystem::LoginUsingToken(const FString& token)
{
	if (GeneratorBridge) {
		return GeneratorBridge->LoginUsingToken(token);
	}
	return false;
}

void UStableDiffusionSubsystem::InitModel(const FStableDiffusionModelOptions& Model, bool Async, bool AllowNSFW, EPaddingMode PaddingMode)
{
	if (GeneratorBridge) {
		// Unload any loaded models first
		//ReleaseModel();

		// Forward image updated event from bridge to subsystem
		this->GeneratorBridge->OnImageProgressEx.AddUniqueDynamic(this, &UStableDiffusionSubsystem::UpdateImageProgress);

		if (Async) {
			AsyncTask(ENamedThreads::AnyBackgroundHiPriTask, [this, Model, AllowNSFW, PaddingMode]() {
				this->ModelInitialised = this->GeneratorBridge->InitModel(Model, AllowNSFW, PaddingMode);
				if (this->ModelInitialised)
					ModelOptions = Model;

				AsyncTask(ENamedThreads::GameThread, [this]() {
					this->OnModelInitializedEx.Broadcast(this->ModelInitialised);
					});
				});
		}
		else {
			this->ModelInitialised = this->GeneratorBridge->InitModel(Model, AllowNSFW, PaddingMode);
			if (this->ModelInitialised)
				ModelOptions = Model;

			AsyncTask(ENamedThreads::GameThread, [this]() {
				this->OnModelInitializedEx.Broadcast(this->ModelInitialised);
			});
		}
	}
}

void UStableDiffusionSubsystem::ReleaseModel()
{
	if (GeneratorBridge) {
		GeneratorBridge->ReleaseModel();
		this->GeneratorBridge->OnImageProgressEx.RemoveDynamic(this, &UStableDiffusionSubsystem::UpdateImageProgress);
		ModelInitialised = false;
	}
}

TSharedPtr<FSceneViewport> UStableDiffusionSubsystem::GetCapturingViewport()
{
	// Find active viewport
	TSharedPtr<FSceneViewport> OutSceneViewport;
#if WITH_EDITOR
	if (GIsEditor)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::Editor)
			{
				if (FModuleManager::Get().IsModuleLoaded("LevelEditor"))
				{
					FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
					TSharedPtr<IAssetViewport> ActiveLevelViewport = LevelEditorModule.GetFirstActiveViewport();
					if (ActiveLevelViewport.IsValid())
					{
						OutSceneViewport = ActiveLevelViewport->GetSharedActiveViewport();
					}
				}
			}
			else if (Context.WorldType == EWorldType::PIE)
			{
				FSlatePlayInEditorInfo* SlatePlayInEditorSession = GEditor->SlatePlayInEditorMap.Find(Context.ContextHandle);
				if (SlatePlayInEditorSession)
				{
					if (SlatePlayInEditorSession->DestinationSlateViewport.IsValid())
					{
						TSharedPtr<IAssetViewport> DestinationLevelViewport = SlatePlayInEditorSession->DestinationSlateViewport.Pin();
						OutSceneViewport = DestinationLevelViewport->GetSharedActiveViewport();
					}
					else if (SlatePlayInEditorSession->SlatePlayInEditorWindowViewport.IsValid())
					{
						OutSceneViewport = SlatePlayInEditorSession->SlatePlayInEditorWindowViewport;
					}
				}
			}
		}
	}
	else
#endif
	{
		UGameEngine* GameEngine = Cast<UGameEngine>(GEngine);
		OutSceneViewport = GameEngine->SceneViewport;
	}

	return OutSceneViewport;
}

void UStableDiffusionSubsystem::StartCapturingViewport()
{
	// Find active viewport
	TSharedPtr<FSceneViewport> OutSceneViewport = GetCapturingViewport();
	SetCaptureViewport(OutSceneViewport.ToSharedRef(), OutSceneViewport->GetSize());
}

void UStableDiffusionSubsystem::SetCaptureViewport(TSharedRef<FSceneViewport> Viewport, FIntPoint FrameSize)
{
	ViewportCapture = MakeShared<FFrameGrabber>(Viewport, FrameSize);
	ViewportCapture->StartCapturingFrames();
}

void UStableDiffusionSubsystem::GenerateImage(FStableDiffusionInput Input, EInputImageSource ImageSourceType)
{
	if (!GeneratorBridge)
		return;

	bIsGenerating = true;

	AsyncTask(ENamedThreads::GameThread, [this, Input, ImageSourceType]() mutable
		{
			// Remember prior screen message state and disable it so our viewport is clean
			bool bPrevGScreenMessagesEnabled = GAreScreenMessagesEnabled;
			bool bPrevViewportGameViewEnabled = false;
			GAreScreenMessagesEnabled = false;
			ULevelEditorSubsystem* LevelEditorSubsystem = nullptr;

#if WITH_EDITOR
			//Only set Game view when streaming in editor mode (so not on PIE, SIE or standalone) 
			if (GEditor && !GEditor->IsPlaySessionInProgress())
			{
				LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
				if (LevelEditorSubsystem)
				{
					bPrevViewportGameViewEnabled = LevelEditorSubsystem->EditorGetGameView();
					LevelEditorSubsystem->EditorSetGameView(true);
				}
			}

#endif
			if (ImageSourceType == EInputImageSource::Viewport) {
				CaptureFromViewportSource(MoveTempIfPossible(Input));
			}
			else if (ImageSourceType == EInputImageSource::SceneCapture2D) {
				CaptureFromSceneCaptureSource(MoveTempIfPossible(Input));
			}

			// Restore screen messages and UI
			GAreScreenMessagesEnabled = bPrevGScreenMessagesEnabled;
			if (LevelEditorSubsystem)
				LevelEditorSubsystem->EditorSetGameView(bPrevViewportGameViewEnabled);
		});
}

void UStableDiffusionSubsystem::StopGeneratingImage()
{
	this->GeneratorBridge->StopImageGeneration();
	bIsGenerating = false;
}

void UStableDiffusionSubsystem::StartImageGeneration(FStableDiffusionInput Input)
{
	// Generate the image on a background thread
	//CurrentRenderTask = TGraphTask<FSDRenderTask>::CreateTask().ConstructAndDispatchWhenReady(ENamedThreads::AnyBackgroundHiPriTask, MoveTemp([this, Input]()
	AsyncTask(ENamedThreads::AnyBackgroundHiPriTask, [this, Input]()
		{
			// Generate image
			FStableDiffusionImageResult result = this->GeneratorBridge->GenerateImageFromStartImage(Input);
			bIsGenerating = false;

			// Create generated texture on game thread
			AsyncTask(ENamedThreads::GameThread, [this, result]
			{
				this->OnImageGenerationCompleteEx.Broadcast(result);
			});
	});
	//);
}

void UStableDiffusionSubsystem::UpsampleImage(const FStableDiffusionImageResult& input)
{
	if (!GeneratorBridge)
		return;

	bIsUpsampling = true;

	AsyncTask(ENamedThreads::AnyBackgroundHiPriTask, [this, input](){
		auto result = GeneratorBridge->UpsampleImage(input);
		AsyncTask(ENamedThreads::GameThread, [this, result=MoveTemp(result)]() {
			bIsUpsampling = false;
			OnImageUpsampleCompleteEx.Broadcast(result);
		});
	});
}

bool UStableDiffusionSubsystem::SaveTextureAsset(const FString& PackagePath, const FString& Name, UTexture2D* Texture, const FStableDiffusionGenerationOptions& ImageInputs, bool Upsampled)
{
	if (Name.IsEmpty() || PackagePath.IsEmpty() || !Texture)
		return false;

	// Create package
	FString FullPackagePath = FPaths::Combine(PackagePath, Name);
	UPackage* Package = CreatePackage(this, *FullPackagePath);
	Package->FullyLoad();

	// Duplicate texture
	auto SrcMipData = Texture->Source.LockMip(0);// GetPlatformMips()[0].BulkData;
	FString TexName = "T_" + Name;
	UTexture2D* NewTexture = NewObject<UTexture2D>(Package, *TexName, RF_Public | RF_Standalone | RF_MarkAsRootSet);
	NewTexture->AddToRoot();
	NewTexture = ColorBufferToTexture(Name, SrcMipData, FIntPoint(Texture->GetSizeX(), Texture->GetSizeY()), NewTexture);
	Texture->Source.UnlockMip(0);

	// Create data asset
	FString AssetName = "DA_" + Name;
	UStableDiffusionImageResultAsset* NewImageResultAsset = NewObject<UStableDiffusionImageResultAsset>(Package, *AssetName, RF_Public | RF_Standalone | RF_MarkAsRootSet);
	NewImageResultAsset->ImageInputs = ImageInputs;
	NewImageResultAsset->Upsampled = Upsampled;
	NewImageResultAsset->ImageOutput = NewTexture;

	// Update package
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewTexture);
	
	// Save texture pacakge
	FString PackageFileName = FPackageName::LongPackageNameToFilename(FullPackagePath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs PackageArgs;
	PackageArgs.TopLevelFlags = EObjectFlags::RF_Public | EObjectFlags::RF_Standalone;
	PackageArgs.bForceByteSwapping = true;
	bool bSaved = UPackage::SavePackage(Package, NewTexture, *PackageFileName, PackageArgs);

	return bSaved;
}

void UStableDiffusionSubsystem::UpdateImageProgress(int32 Step, int32 Timestep, float Progress, FIntPoint Size, const TArray<FColor>& PixelData)
{
	OnImageProgressUpdated.Broadcast(Step, Timestep, Progress, Size, PixelData);
}

void UStableDiffusionSubsystem::SetLivePreviewEnabled(bool Enabled, float Delay, USceneCaptureComponent2D* Source)
{
	if (Enabled){
		if (Source) {
			if (!OnCaptureCameraUpdatedDlgHandle.IsValid()) {
				// Handle when capture component moves
				OnCaptureCameraUpdatedDlgHandle = Source->TransformUpdated.AddLambda([this, Delay, Source](USceneComponent* UpdatedComponent, EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport) {
				
				FEditorCameraLivePreview CameraInfo;
				CameraInfo.Location = UpdatedComponent->GetComponentTransform().GetLocation();
				CameraInfo.Rotation = UpdatedComponent->GetComponentTransform().GetRotation().Rotator();
				CameraInfo.ViewportType = Source->ProjectionType == ECameraProjectionMode::Type::Perspective ? ELevelViewportType::LVT_Perspective : ELevelViewportType::LVT_OrthoFreelook;
				CameraInfo.ViewportIndex = 0;

				UE_LOG(LogTemp, Log, TEXT("Moving capture component to %s %s"), *CameraInfo.Location.ToString(), *CameraInfo.Rotation.ToString())

					// Only broadcast when the camera is not moving
					if (!(LastPreviewCameraInfo == CameraInfo)) {
						GEditor->GetTimerManager()->SetTimer(IdleCameraTimer, this, &UStableDiffusionSubsystem::LivePreviewUpdate, Delay, false);
					}

				LastPreviewCameraInfo = CameraInfo;
					});
			}
		}
		else {
			if (!OnCaptureCameraUpdatedDlgHandle.IsValid()) {
				OnCaptureCameraUpdatedDlgHandle = FEditorDelegates::OnEditorCameraMoved.AddLambda([this, Delay](const FVector& Location, const FRotator& Rotation, ELevelViewportType ViewportType, int32 ViewportIndex) {
					UE_LOG(LogTemp, Log, TEXT("Moving editor camera to %s %s"), *Location.ToString(), *Rotation.ToString())

					FEditorCameraLivePreview CameraInfo;
					CameraInfo.Location = Location;
					CameraInfo.Rotation = Rotation;
					CameraInfo.ViewportType = ViewportType;
					CameraInfo.ViewportIndex = ViewportIndex;

					// Only broadcast when the camera is not moving
					if (!(LastPreviewCameraInfo == CameraInfo)) {
						GEditor->GetTimerManager()->SetTimer(IdleCameraTimer, this, &UStableDiffusionSubsystem::LivePreviewUpdate, Delay, false);
					}

					LastPreviewCameraInfo = CameraInfo;
				});
			}
		}
		
	}
	else if (!Enabled) {
		if (Source) {
			Source->TransformUpdated.Remove(OnCaptureCameraUpdatedDlgHandle);
		}
		else {
			FEditorDelegates::OnEditorCameraMoved.Remove(OnCaptureCameraUpdatedDlgHandle);
		}
		if (OnCaptureCameraUpdatedDlgHandle.IsValid()) {
			OnCaptureCameraUpdatedDlgHandle.Reset();
		}
	}
}

UTextureRenderTarget2D* UStableDiffusionSubsystem::SetLivePreviewForLayer(FIntPoint Size, ULayerProcessorBase* Layer, USceneCaptureComponent2D* CaptureSource)
{
	check(Layer);

	if (PreviewedLayer->IsValidLowLevel()) {
		DisableLivePreviewForLayer();
	}
	PreviewedLayer = Layer;

	USceneCaptureComponent2D* ActiveCaptureComponent = nullptr;
	
	// Assign or create the capture source
	if (CaptureSource->IsValidLowLevel()) {
		ActiveCaptureComponent = CaptureSource;
	}
	else {
		if (!LayerPreviewCapture.SceneCapture) {
			LayerPreviewCapture = CreateSceneCaptureCamera();
			//DepthPreviewCapture.SceneCapture->SetIsTemporarilyHiddenInEditor(true);
			
			OnLayerPreviewUpdateHandle = FEditorDelegates::OnEditorCameraMoved.AddLambda([this](const FVector& Location, const FRotator& Rotation, ELevelViewportType ViewportType, int32 ViewportIndex) {
				UpdateSceneCaptureCamera(LayerPreviewCapture);
			});
		}
		ActiveCaptureComponent = LayerPreviewCapture.SceneCapture->GetCaptureComponent2D();
	}

	// Start capturing the scene
	PreviewedLayer->BeginCaptureLayer(Size, LayerPreviewCapture.SceneCapture->GetCaptureComponent2D());
	PreviewedLayer->CaptureLayer(CaptureSource, false);
	return PreviewedLayer->RenderTarget;
}

void UStableDiffusionSubsystem::DisableLivePreviewForLayer()
{
	if (LayerPreviewCapture.SceneCapture && PreviewedLayer->IsValidLowLevel()) {
		AsyncTask(ENamedThreads::GameThread, [this]() {
			if(PreviewedLayer)
				PreviewedLayer->EndCaptureLayer(LayerPreviewCapture.SceneCapture->GetCaptureComponent2D());

			// Remove camera updater
			FEditorDelegates::OnEditorCameraMoved.Remove(OnLayerPreviewUpdateHandle);
			OnLayerPreviewUpdateHandle.Reset();

			LayerPreviewCapture.SceneCapture->Destroy();
			LayerPreviewCapture.SceneCapture = nullptr;
		});
	}

	PreviewedLayer = nullptr;
}

//UTextureRenderTarget2D* UStableDiffusionSubsystem::EnableDepthPreview(USceneCaptureComponent2D* CaptureComponent, float SceneDepthScale, float SceneDepthOffset, FIntPoint ViewportSize)
//{
//	USceneCaptureComponent2D* ActiveCaptureComponent = nullptr;
//	
//	if (CaptureComponent) {
//		ActiveCaptureComponent = CaptureComponent;
//	}
//	else {
//		if (!DepthPreviewCapture.SceneCapture) {
//			DepthPreviewCapture = CreateSceneCaptureCamera();
//			//DepthPreviewCapture.SceneCapture->SetIsTemporarilyHiddenInEditor(true);
//			
//			OnDepthPreviewUpdateHandle = FEditorDelegates::OnEditorCameraMoved.AddLambda([this](const FVector& Location, const FRotator& Rotation, ELevelViewportType ViewportType, int32 ViewportIndex) {
//				UpdateSceneCaptureCamera(DepthPreviewCapture);
//			});
//		}
//		ActiveCaptureComponent = DepthPreviewCapture.SceneCapture->GetCaptureComponent2D();
//	}
//
//	// Create render target to hold our scene capture data
//	UTextureRenderTarget2D* DepthPreviewRT = NewObject<UTextureRenderTarget2D>(ActiveCaptureComponent);
//	check(DepthPreviewRT);
//	DepthPreviewRT->InitCustomFormat(ViewportSize.X, ViewportSize.Y, PF_R8G8B8A8, false);
//	DepthPreviewRT->UpdateResourceImmediate(true);
//	ActiveCaptureComponent->TextureTarget = DepthPreviewRT;
//	FTextureRenderTargetResource* FullFrameRT_TexRes = DepthPreviewRT->GameThread_GetRenderTargetResource();
//
//	// Create material to render depth postprocess mat
//	TSoftObjectPtr<UMaterialInterface> DepthMatRef = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(DepthMaterialAsset));
//	auto DepthMaterial = DepthMatRef.LoadSynchronous();
//	UMaterialInstanceDynamic* DepthMatInst = UMaterialInstanceDynamic::Create(DepthMaterial, DepthPreviewCapture.SceneCapture);
//	DepthMatInst->SetScalarParameterValue("DepthScale", SceneDepthScale);
//	DepthMatInst->SetScalarParameterValue("StartDepth", SceneDepthOffset);
//	ActiveCaptureComponent->AddOrUpdateBlendable(DepthMatInst);
//
//	// Capture the depth map
//	ActiveCaptureComponent->CaptureScene();
//
//	return DepthPreviewRT;
//}

//void UStableDiffusionSubsystem::DisableDepthPreview()
//{
//	if (DepthPreviewCapture.SceneCapture) {
//		AsyncTask(ENamedThreads::GameThread, [this]() {
//			// Remove camera updater
//			FEditorDelegates::OnEditorCameraMoved.Remove(OnDepthPreviewUpdateHandle);
//			OnDepthPreviewUpdateHandle.Reset();
//
//			DepthPreviewCapture.SceneCapture->Destroy();
//			DepthPreviewCapture.SceneCapture = nullptr;
//		});
//	}
//}

UTexture2D* UStableDiffusionSubsystem::ColorBufferToTexture(const FString& FrameName, const TArray<FColor>& FrameColors, const FIntPoint& FrameSize, UTexture2D* OutTex)
{
	if (!FrameColors.Num())
		return nullptr;
	return ColorBufferToTexture(FrameName, (uint8*)FrameColors.GetData(), FrameSize, OutTex);
}

FViewportSceneCapture UStableDiffusionSubsystem::CreateSceneCaptureCamera()
{
	FViewportSceneCapture SceneCapture;

	bool FoundViewport = false;
	FVector CameraLocation = FVector::ZeroVector;
	FRotator CameraRotation = FRotator::ZeroRotator;
	float HorizFov = 0.0f;

	for (FLevelEditorViewportClient* LevelVC : GEditor->GetLevelViewportClients())
	{
		if (LevelVC && LevelVC->IsPerspective())
		{
			FoundViewport = true;
			SceneCapture.ViewportClient = LevelVC;
			break;
		}
	}

	if (FoundViewport){
		SceneCapture.SceneCapture = GEditor->GetEditorWorldContext().World()->SpawnActor<ASceneCapture2D>();
		SceneCapture.SceneCapture->GetCaptureComponent2D()->bCaptureEveryFrame = true;
		SceneCapture.SceneCapture->GetCaptureComponent2D()->bCaptureOnMovement = false;
		SceneCapture.SceneCapture->GetCaptureComponent2D()->bAlwaysPersistRenderingState = true;
		SceneCapture.SceneCapture->GetCaptureComponent2D()->CompositeMode = SCCM_Overwrite;
		SceneCapture.SceneCapture->GetCaptureComponent2D()->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;//ESceneCaptureSource::SCS_FinalToneCurveHDR;
		UpdateSceneCaptureCamera(SceneCapture);
	}

	return SceneCapture;
}

void UStableDiffusionSubsystem::UpdateSceneCaptureCamera(FViewportSceneCapture& SceneCapture)
{
	SceneCapture.SceneCapture->SetActorLocation(SceneCapture.ViewportClient->GetViewLocation());
	SceneCapture.SceneCapture->SetActorRotation(SceneCapture.ViewportClient->GetViewRotation());
	

	auto CaptureComponent = SceneCapture.SceneCapture->GetCaptureComponent2D();
	CaptureComponent->FOVAngle = SceneCapture.ViewportClient->FOVAngle;

	// Update any previewed layers
	if (PreviewedLayer->IsValidLowLevel() && LayerPreviewCapture.SceneCapture->IsValidLowLevel())
		PreviewedLayer->CaptureLayer(LayerPreviewCapture.SceneCapture->GetCaptureComponent2D(), false);

}

void UStableDiffusionSubsystem::CaptureFromViewportSource(FStableDiffusionInput Input)
{
	auto ViewportSize = GetCapturingViewport()->GetSizeXY();

	// Make sure viewport capture objects are available
	StartCapturingViewport();

	// Process each layer the model has requested
	if (ModelOptions.Layers.Num()) {
		Input.ProcessedLayers.Reset();
		Input.ProcessedLayers.Reserve(ModelOptions.Layers.Num());
		
		// Create a scene capture
		auto SceneCapture = CreateSceneCaptureCamera();

		for (auto& Layer : ModelOptions.Layers) {
			// Copy layer
			FLayerData TargetLayer = Layer;
			TargetLayer.Processor->BeginCaptureLayer(ViewportSize, SceneCapture.SceneCapture->GetCaptureComponent2D());
			TargetLayer.Processor->CaptureLayer(SceneCapture.SceneCapture->GetCaptureComponent2D());
			TargetLayer.Processor->EndCaptureLayer(SceneCapture.SceneCapture->GetCaptureComponent2D());
			TargetLayer.LayerPixels = TargetLayer.Processor->ProcessLayer(TargetLayer.Processor->RenderTarget);
			Input.ProcessedLayers.Add(MoveTemp(TargetLayer));
		}

		// Cleanup
		SceneCapture.SceneCapture->Destroy();
	}

	// Create a frame payload we will wait on to be filled with a frame
	auto framePtr = MakeShared<FCapturedFramePayload>();
	framePtr->OnFrameCapture.AddLambda([=](FColor* Pixels, FIntPoint BufferSize, FIntPoint TargetSize) mutable {
		// Copy frame data
		TArray<FColor> CopiedFrame = CopyFrameData(TargetSize, BufferSize, Pixels);
		
		// Find a final colour layer as a destination for our captured frame
		auto FinalColorProcessor = Input.ProcessedLayers.FindByPredicate([](const FLayerData& Layer) { return Layer.Processor->IsA<UFinalColorLayerProcessor>(); });
		if (FinalColorProcessor) {
			FinalColorProcessor->LayerPixels = MoveTemp(CopiedFrame);
		}

		// Don't need to keep capturing whilst generating
		ViewportCapture->StopCapturingFrames();

		// Set size from viewport
		Input.Options.InSizeX = ViewportSize.X;
		Input.Options.InSizeY = ViewportSize.Y;

		// Only start image generation when we have a frame
		StartImageGeneration(Input);
	});

	// Start frame capture
	ViewportCapture->CaptureThisFrame(framePtr);
}

void UStableDiffusionSubsystem::CaptureFromSceneCaptureSource(FStableDiffusionInput Input)
{
	// Use chosen scene capture component or create a default one
	USceneCaptureComponent2D* CaptureComponent = nullptr;
	if (!Input.CaptureSource) {
		// Create a default SceneCapture2D that will capture our editor viewport
		CurrentSceneCapture = CreateSceneCaptureCamera();
		CaptureComponent = CurrentSceneCapture.SceneCapture->GetCaptureComponent2D();
	}
	else {
		CaptureComponent = Input.CaptureSource;
	}

	// Get the capture size from the source
	auto ViewportSize = GetCapturingViewport()->GetSizeXY();
	FIntPoint CaptureSize = (CaptureComponent->TextureTarget) ? FIntPoint(CaptureComponent->TextureTarget->SizeX, CaptureComponent->TextureTarget->SizeY) : ViewportSize;
	
	// Process each layer the model has requested
	Input.ProcessedLayers.Reset();
	Input.ProcessedLayers.Reserve(ModelOptions.Layers.Num());
	for (auto Layer : ModelOptions.Layers) {
		Layer.Processor->BeginCaptureLayer(CaptureSize, CaptureComponent);
		Layer.Processor->CaptureLayer(CaptureComponent);
		Layer.Processor->EndCaptureLayer(CaptureComponent);
		Layer.LayerPixels = Layer.Processor->ProcessLayer(Layer.Processor->RenderTarget);
		Input.ProcessedLayers.Add(MoveTemp(Layer));
	}

	// Set size from scene capture
	Input.Options.InSizeX = CaptureSize.X;
	Input.Options.InSizeY = CaptureSize.Y;

	if (!Input.CaptureSource && this->CurrentSceneCapture.SceneCapture) {
		// Cleanup created scene capture once we've captured all our pixel data
		this->CurrentSceneCapture.SceneCapture->Destroy();
		this->CurrentSceneCapture.ViewportClient = nullptr;
	}
	else {
		
	}

	StartImageGeneration(Input);
}


UTexture2D* UStableDiffusionSubsystem::ColorBufferToTexture(const FString& FrameName, const uint8* FrameData, const FIntPoint& FrameSize, UTexture2D* OutTex)
{
	if (!FrameData) 
		return nullptr;

	if (!OutTex) {
		TObjectPtr<UTexture2D> NewTex = UTexture2D::CreateTransient(FrameSize.X, FrameSize.Y, EPixelFormat::PF_B8G8R8A8);
		OutTex = NewTex;
	}

	OutTex->Source.Init(FrameSize.X, FrameSize.Y, 1, 1, ETextureSourceFormat::TSF_BGRA8);//ETextureSourceFormat::TSF_RGBA8);
	OutTex->MipGenSettings = TMGS_NoMipmaps;
	OutTex->SRGB = true;
	OutTex->DeferCompression = true;

	uint8* TextureData = OutTex->Source.LockMip(0);
	FMemory::Memcpy(TextureData, FrameData, sizeof(uint8) * FrameSize.X * FrameSize.Y * 4);
	OutTex->Source.UnlockMip(0);
	OutTex->UpdateResource();

#if WITH_EDITOR
	OutTex->PostEditChange();
#endif
	return OutTex;
}

void UStableDiffusionSubsystem::OnLivePreviewCheckUpdate(USceneComponent* UpdatedComponent, EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport) {

}

void UStableDiffusionSubsystem::LivePreviewUpdate()
{
	OnEditorCameraMovedEx.Broadcast(LastPreviewCameraInfo);
}

FString UStableDiffusionSubsystem::OpenImageFilePicker(const FString& StartDir)
{
	FString OpenFolderName;
	bool bOpen = false;

	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();	
	if (DesktopPlatform)
	{
		FString ExtensionStr;
		ExtensionStr += TEXT("Image file (*.png)|*.exr|*.bmp|*.exr");
		bOpen = DesktopPlatform->OpenDirectoryDialog(
			FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
			"Save image to destination...",
			StartDir,
			OpenFolderName
		);
	}
	if (!bOpen)
	{
		return "";
	}

	return OpenFolderName;
}

FString UStableDiffusionSubsystem::FilepathToLongPackagePath(const FString& Path) 
{
	FString result;
	FString error;
	FPackageName::TryConvertFilenameToLongPackageName(Path, result, &error);
	return result;
}


TArray<FColor> UStableDiffusionSubsystem::CopyFrameData(FIntPoint TargetSize, FIntPoint BufferSize, FColor* ColorBuffer)
{
	// Copy frame data
	TArray<FColor> CopiedFrame;

	CopiedFrame.InsertUninitialized(0, TargetSize.X * TargetSize.Y);
	FColor* Dest = &CopiedFrame[0];
	const int32 MaxWidth = FMath::Min(TargetSize.X, BufferSize.X);
	for (int32 Row = 0; Row < FMath::Min(TargetSize.Y, BufferSize.Y); ++Row)
	{
		FMemory::Memcpy(Dest, ColorBuffer, sizeof(FColor) * MaxWidth);
		ColorBuffer += BufferSize.X;
		Dest += MaxWidth;
	}

	return std::move(CopiedFrame);
}


#undef LOCTEXT_NAMESPACE
