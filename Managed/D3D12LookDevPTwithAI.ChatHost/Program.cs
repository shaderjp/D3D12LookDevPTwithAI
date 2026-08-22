using D3D12LookDevPTwithAI.Chat.Core;
using D3D12LookDevPTwithAI.Chat.Infrastructure;
using D3D12LookDevPTwithAI.ChatHost;
using D3D12LookDevPTwithAI.ChatHost.Inference;
using D3D12LookDevPTwithAI.ChatHost.Mcp;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;

#if DEBUG
const string testRuntimeEnvironmentVariable = "D3D12LOOKDEVPT_AI_TEST_RUNTIME";
var useDeterministicTestRuntime = string.Equals(
    Environment.GetEnvironmentVariable(testRuntimeEnvironmentVariable),
    "deterministic",
    StringComparison.Ordinal);
#endif

if (!CommandLineOptions.TryParse(args, out var options, out _))
{
    Environment.ExitCode = 2;
    return;
}

var builder = Host.CreateApplicationBuilder();
builder.Logging.ClearProviders();
builder.Services.Configure<HostOptions>(hostOptions =>
    hostOptions.ShutdownTimeout = ChatHostShutdownBudget.HostTimeout);
builder.Services.AddSingleton(options!);
builder.Services.AddSingleton<IAppPaths>(_ => new AppPaths());
builder.Services.AddSingleton<IConversationStore, SqliteConversationStore>();
builder.Services.AddSingleton<
    ILocalInferenceSettingsProvider,
    JsonLocalInferenceSettingsProvider>();
builder.Services.AddSingleton<ILlamaServerSessionProvider>(serviceProvider =>
    new LlamaServerProcessSessionProvider(
        serviceProvider.GetRequiredService<ILocalInferenceSettingsProvider>()));
builder.Services.AddSingleton<IChatInferenceRuntime>(serviceProvider =>
#if DEBUG
    useDeterministicTestRuntime
        ? new DeterministicChatInferenceRuntime()
        :
#endif
        new LlamaServerChatInferenceRuntime(
            serviceProvider.GetRequiredService<ILlamaServerSessionProvider>()));
builder.Services.AddSingleton<NamedPipeConnection>();
#if DEBUG
if (!useDeterministicTestRuntime)
{
#endif
    builder.Services.AddSingleton<ISameInstanceMcpClientFactory>(_ =>
        new SameInstanceMcpClientFactory(options!.ParentProcessId));
#if DEBUG
}
#endif
builder.Services.AddSingleton<ChatCoordinator>();
builder.Services.AddSingleton<PipeRequestRouter>();
builder.Services.AddHostedService<ChatHostWorker>();

var host = builder.Build();
await using var asyncHost = (IAsyncDisposable)host;
await host.RunAsync();
