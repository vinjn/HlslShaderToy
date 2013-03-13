#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <d3dx11.h>
#include <d3dcompiler.h>
#include <xnamath.h>
#include <atlbase.h>
#include <string>
#include <fstream>
#include "V.h"

HRESULT hr = S_OK;

const char kAppName[] = "HlslShaderToy";
const int kAppWidth = 800;
const int kAppHeight = 600;
const int kSrvCount = 4;

const std::string kVertexShaderCode =
"struct VS_Output\n"
"{  \n"
"    float4 pos : SV_POSITION;              \n"
"    float2 tex : TEXCOORD0;\n"
"};\n"

"VS_Output VS(uint id : SV_VertexID)\n"
"{\n"
"    VS_Output output;\n"
"    output.tex = float2((id << 1) & 2, id & 2);\n"
"    output.pos = float4(output.tex * float2(2,-2) + float2(-1,1), 0, 1);\n"
"    return output;\n"
"}\n"
;

const std::string kPixelShaderCommonCode = 
"Texture2D iChannel[4] : register( t0 );\n"

"SamplerState samLinear : register( s0 );\n"

"cbuffer cbNeverChanges : register( b0 )\n"
"{\n"
"    float3      iResolution;     // viewport resolution (in pixels)\n"
"    float       iGlobalTime;     // shader playback time (in seconds)\n"
"    float       iChannelTime[4]; // channel playback time (in seconds)\n"
"    float4      iMouse;          // mouse pixel coords. xy: current (if MLB down), zw: click\n"
"    float4      iDate;           // (year, month, day, time in seconds)\n"
"};\n"

"struct PS_Input\n"
"{\n"
"    float4 pos : SV_POSITION;\n"
"    float2 tex : TEXCOORD0;\n"
"};\n"
;

//--------------------------------------------------------------------------------------
// Structures
//--------------------------------------------------------------------------------------

struct CBOneFrame
{
    XMFLOAT3    iResolution;     // viewport resolution (in pixels)
    float       iGlobalTime;     // shader playback time (in seconds)
    float       iChannelTime[4]; // channel playback time (in seconds)
    XMFLOAT4    iMouse;          // mouse pixel coords. xy: current (if MLB down), zw: click
    XMFLOAT4    iDate;           // (year, month, day, time in seconds)
}g_cbOneFrame;

//--------------------------------------------------------------------------------------
// Global Variables
//--------------------------------------------------------------------------------------
HINSTANCE                               g_hInst;
HWND                                    g_hWnd;
D3D_DRIVER_TYPE                         g_driverType;
D3D_FEATURE_LEVEL                       g_featureLevel;
CComPtr<ID3D11Device>                   g_pd3dDevice;
CComPtr<ID3D11DeviceContext>            g_pImmediateContext;
CComPtr<IDXGISwapChain>                 g_pSwapChain;
CComPtr<ID3D11RenderTargetView>         g_pRenderTargetView;
CComPtr<ID3D11VertexShader>             g_pVertexShader;
CComPtr<ID3D11PixelShader>              g_pPixelShader;
CComPtr<ID3D11Buffer>                   g_pCBOneFrame;
CComPtr<ID3D11ShaderResourceView>       g_pTextureRV;
CComPtr<ID3D11SamplerState>             g_pSamplerLinear;

std::string                             g_pixelShaderFileName;

//--------------------------------------------------------------------------------------
// Forward declarations
//--------------------------------------------------------------------------------------
HRESULT InitWindow( HINSTANCE hInstance, int nCmdShow );
HRESULT InitDevice();
void CleanupDevice();
LRESULT CALLBACK    WndProc( HWND, UINT, WPARAM, LPARAM );
void Render();

//--------------------------------------------------------------------------------------
// Entry point to the program. Initializes everything and goes into a message processing 
// loop. Idle time is used to render the scene.
//--------------------------------------------------------------------------------------
int WINAPI WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow )
{
    UNREFERENCED_PARAMETER( hPrevInstance );
    UNREFERENCED_PARAMETER( lpCmdLine );

    if (strlen(lpCmdLine) == 0)
    {
        MessageBox(NULL, "Usage: HlslShaderToy.exe /path/to/pixel_shader.hlsl", kAppName, MB_OK);
        return -1;
    }
    else
    {
        g_pixelShaderFileName = lpCmdLine;
        if (g_pixelShaderFileName[0] == '"')
            g_pixelShaderFileName = g_pixelShaderFileName.substr(1, g_pixelShaderFileName.length()-2);
    }

    if( FAILED( InitWindow( hInstance, nCmdShow ) ) )
        return 0;

    if( FAILED( InitDevice() ) )
    {
        CleanupDevice();
        return 0;
    }

    // Main message loop
    MSG msg = {0};
    while( WM_QUIT != msg.message )
    {
        if( PeekMessage( &msg, NULL, 0, 0, PM_REMOVE ) )
        {
            TranslateMessage( &msg );
            DispatchMessage( &msg );
        }
        else
        {
            Render();
        }
    }

    CleanupDevice();

    return ( int )msg.wParam;
}


//--------------------------------------------------------------------------------------
// Register class and create window
//--------------------------------------------------------------------------------------
HRESULT InitWindow( HINSTANCE hInstance, int nCmdShow )
{
    // Register class
    WNDCLASSEX wcex = {sizeof( WNDCLASSEX )};
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor( NULL, IDC_ARROW );
    wcex.hbrBackground = ( HBRUSH )( COLOR_WINDOW + 1 );
    wcex.lpszClassName = kAppName;
    if( !RegisterClassEx( &wcex ) )
        return E_FAIL;

    // Create window
    g_hInst = hInstance;
    RECT rc = { 0, 0, kAppWidth, kAppHeight};
    AdjustWindowRect( &rc, WS_OVERLAPPEDWINDOW, FALSE );
    g_hWnd = CreateWindow( kAppName, kAppName, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hInstance,
        NULL );
    if( !g_hWnd )
        return E_FAIL;

    ShowWindow( g_hWnd, nCmdShow );

    return S_OK;
}

HRESULT CreatePixelShaderFromFile(LPCSTR filename)
{
    // open
    std::ifstream ifs(filename, std::ifstream::binary);
    if (!ifs)
    {
        return E_FAIL;
    }

    // get file content
    ifs.seekg (0, ifs.end);
    size_t length = ifs.tellg();
    ifs.seekg (0, ifs.beg);
    char* fileContent = new char[length+1];
    ifs.read(&fileContent[0], length);
    fileContent[length] = '\0';

    // add together
    std::string psText = kPixelShaderCommonCode + fileContent;

    delete[] fileContent;

    ID3DBlob* pPSBlob = NULL;
    V_RETURN(CompileShaderFromMemory( psText.c_str(), "main", "ps_4_0", &pPSBlob ));

    g_pPixelShader = NULL;
    V_RETURN(g_pd3dDevice->CreatePixelShader( pPSBlob->GetBufferPointer(), pPSBlob->GetBufferSize(), NULL, &g_pPixelShader ));

    return hr;
}

//--------------------------------------------------------------------------------------
// Create Direct3D device and swap chain
//--------------------------------------------------------------------------------------
HRESULT InitDevice()
{
    RECT rc;
    GetClientRect( g_hWnd, &rc );
    UINT width = rc.right - rc.left;
    UINT height = rc.bottom - rc.top;

    g_cbOneFrame.iResolution.x = (float)width;
    g_cbOneFrame.iResolution.y = (float)height;
    g_cbOneFrame.iResolution.z = 1.0f;

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_DRIVER_TYPE driverTypes[] =
    {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
        D3D_DRIVER_TYPE_REFERENCE,
    };
    UINT numDriverTypes = ARRAYSIZE( driverTypes );

    D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    UINT numFeatureLevels = ARRAYSIZE( featureLevels );

    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory( &sd, sizeof( sd ) );
    sd.BufferCount = 1;
    sd.BufferDesc.Width = width;
    sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;

    for( UINT driverTypeIndex = 0; driverTypeIndex < numDriverTypes; driverTypeIndex++ )
    {
        g_driverType = driverTypes[driverTypeIndex];
        hr = D3D11CreateDeviceAndSwapChain( NULL, g_driverType, NULL, createDeviceFlags, featureLevels, numFeatureLevels,
            D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &g_featureLevel, &g_pImmediateContext );
        if( SUCCEEDED( hr ) )
            break;
    }
    if( FAILED( hr ) )
        return hr;

    // Create a render target view
    ID3D11Texture2D* pBackBuffer = NULL;
    V_RETURN(g_pSwapChain->GetBuffer( 0, __uuidof( ID3D11Texture2D ), ( LPVOID* )&pBackBuffer ));

    hr = g_pd3dDevice->CreateRenderTargetView( pBackBuffer, NULL, &g_pRenderTargetView );
    pBackBuffer->Release();
    if( FAILED( hr ) )
        return hr;

    ID3D11RenderTargetView* pRTVs[] = {g_pRenderTargetView};
    g_pImmediateContext->OMSetRenderTargets( 1, pRTVs, NULL );

    // Setup the viewport
    CD3D11_VIEWPORT vp(0.0f, 0.0f, (float)width, (float)height);
    g_pImmediateContext->RSSetViewports( 1, &vp );

    {
        ID3DBlob* pVSBlob = NULL;
        V_RETURN(CompileShaderFromMemory( kVertexShaderCode.c_str(), "VS", "vs_4_0", &pVSBlob ));
        V_RETURN(g_pd3dDevice->CreateVertexShader( pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize(), NULL, &g_pVertexShader ));
    }

    V_RETURN(CreatePixelShaderFromFile(g_pixelShaderFileName.c_str()));

    {
        CD3D11_BUFFER_DESC desc(sizeof(CBOneFrame), D3D11_BIND_CONSTANT_BUFFER);
        V_RETURN(g_pd3dDevice->CreateBuffer( &desc, NULL, &g_pCBOneFrame ));
    }

    // Load the Texture
    V_RETURN(D3DX11CreateShaderResourceViewFromFile( g_pd3dDevice, "../media/photo_4.jpg", NULL, NULL, &g_pTextureRV, NULL ));

    // Create the sample state
    CD3D11_SAMPLER_DESC sampDesc(D3D11_DEFAULT);
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    V_RETURN(g_pd3dDevice->CreateSamplerState( &sampDesc, &g_pSamplerLinear ));

    return S_OK;
}


//--------------------------------------------------------------------------------------
// Clean up the objects we've created
//--------------------------------------------------------------------------------------
void CleanupDevice()
{
    if( g_pImmediateContext ) g_pImmediateContext->ClearState();
}


//--------------------------------------------------------------------------------------
// Called every time the application receives a message
//--------------------------------------------------------------------------------------
LRESULT CALLBACK WndProc( HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam )
{
    PAINTSTRUCT ps;
    HDC hdc;

    int mouseX = GET_X_LPARAM(lParam);
    int mouseY = GET_Y_LPARAM(lParam);

    switch( message )
    {
    case WM_PAINT:
        {
            hdc = BeginPaint( hWnd, &ps );
            EndPaint( hWnd, &ps );
        }break;
    case WM_KEYUP:
        {

        }break;
    case WM_MOUSEMOVE:
        {
            g_cbOneFrame.iMouse.x = (float)mouseX;
            g_cbOneFrame.iMouse.y = (float)mouseY;
        }break;
    case WM_DESTROY:
        PostQuitMessage( 0 );
        break;
    default:
        return DefWindowProc( hWnd, message, wParam, lParam );
    }

    return 0;
}


//--------------------------------------------------------------------------------------
// Render a frame
//--------------------------------------------------------------------------------------
void Render()
{
    static DWORD dwTimeStart = GetTickCount();
    g_cbOneFrame.iGlobalTime = ( GetTickCount() - dwTimeStart ) / 1000.0f;

    {
        float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // red, green, blue, alpha
        g_pImmediateContext->ClearRenderTargetView( g_pRenderTargetView, ClearColor );
    }

    g_pImmediateContext->UpdateSubresource( g_pCBOneFrame, 0, NULL, &g_cbOneFrame, 0, 0 );

    g_pImmediateContext->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    g_pImmediateContext->VSSetShader( g_pVertexShader, NULL, 0 );

    g_pImmediateContext->PSSetShader( g_pPixelShader, NULL, 0 );
    ID3D11Buffer* pCBuffers[] = {g_pCBOneFrame};
    g_pImmediateContext->PSSetConstantBuffers( 0, 1, pCBuffers );
    ID3D11ShaderResourceView* pSRVs[kSrvCount] = {g_pTextureRV};
    g_pImmediateContext->PSSetShaderResources( 0, kSrvCount, pSRVs );
    ID3D11SamplerState* pSamplers[] = {g_pSamplerLinear};
    g_pImmediateContext->PSSetSamplers( 0, 1, pSamplers );

    g_pImmediateContext->Draw( 3, 0 );

    g_pSwapChain->Present( 0, 0 );
}