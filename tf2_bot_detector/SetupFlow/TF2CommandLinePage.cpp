#include "TF2CommandLinePage.h"
#include "Config/Settings.h"
#include "ImGui_TF2BotDetector.h"
#include "Log.h"
#include "Platform/Platform.h"

#include <srcon/srcon.h>
#include <mh/text/charconv_helper.hpp>
#include <mh/text/string_insertion.hpp>

#include <chrono>
#include <random>

using namespace std::chrono_literals;
using namespace std::string_literals;
using namespace tf2_bot_detector;

#ifdef _DEBUG
namespace tf2_bot_detector
{
	extern uint32_t g_StaticRandomSeed;
}
#endif

static std::string GenerateRandomRCONPassword(size_t length = 16)
{
	std::mt19937 generator;
	{
		std::random_device randomSeed;
		generator.seed(randomSeed());
	}

	constexpr char PALETTE[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
	std::uniform_int_distribution<size_t> dist(0, std::size(PALETTE) - 1);

	std::string retVal(length, '\0');
	for (size_t i = 0; i < length; i++)
		retVal[i] = PALETTE[dist(generator)];

	return retVal;
}

static uint16_t GenerateRandomRCONPort()
{
	std::mt19937 generator;
	{
		std::random_device randomSeed;
		generator.seed(randomSeed());
	}

	// Some routers have issues handling high port numbers. By restricting
	// ourselves to these high port numbers, we add another layer of security.
	std::uniform_int_distribution<uint16_t> dist(40000, 65535);
	return dist(generator);
}

void TF2CommandLinePage::Data::TryUpdateCmdlineArgs()
{
	if (m_CommandLineArgsFuture.valid() &&
		m_CommandLineArgsFuture.wait_for(0s) == std::future_status::ready)
	{
		const auto& args = m_CommandLineArgsFuture.get();
		m_MultipleInstances = args.size() > 1;
		if (!m_MultipleInstances)
		{
			if (args.empty())
				m_CommandLineArgs.reset();
			else
				m_CommandLineArgs = TF2CommandLine::Parse(args.at(0));
		}

		m_CommandLineArgsFuture = {};
		m_AtLeastOneUpdateRun = true;
	}

	if (!m_CommandLineArgsFuture.valid())
	{
		// See about starting a new update

		const auto curTime = clock_t::now();
		if (!m_AtLeastOneUpdateRun || (curTime >= (m_LastCLUpdate + CL_UPDATE_INTERVAL)))
		{
			m_CommandLineArgsFuture = Processes::GetTF2CommandLineArgsAsync();
			m_LastCLUpdate = curTime;
		}
	}
}

bool TF2CommandLinePage::ValidateSettings(const Settings& settings) const
{
	if (!Processes::IsTF2Running())
		return false;
	if (!m_Data.m_CommandLineArgs->IsPopulated())
		return false;

	return true;
}

auto TF2CommandLinePage::TF2CommandLine::Parse(const std::string_view& cmdLine) -> TF2CommandLine
{
	const auto args = Shell::SplitCommandLineArgs(cmdLine);

	TF2CommandLine cli{};

	for (size_t i = 0; i < args.size(); i++)
	{
		if (i < (args.size() - 1))
		{
			// We have at least one more arg
			if (cli.m_IP.empty() && args[i] == "+ip")
			{
				cli.m_IP = args[++i];
				continue;
			}
			else if (cli.m_RCONPassword.empty() && args[i] == "+rcon_password")
			{
				cli.m_RCONPassword = args[++i];
				continue;
			}
			else if (!cli.m_RCONPort.has_value() && args[i] == "+hostport")
			{
				if (uint16_t parsedPort; mh::from_chars(args[i + 1], parsedPort))
				{
					cli.m_RCONPort = parsedPort;
					i++;
					continue;
				}
			}
			else if (args[i] == "-usercon")
			{
				cli.m_UseRCON = true;
				continue;
			}
		}
	}

	return cli;
}

bool TF2CommandLinePage::TF2CommandLine::IsPopulated() const
{
	if (!m_UseRCON)
		return false;
	if (m_IP.empty())
		return false;
	if (m_RCONPassword.empty())
		return false;
	if (!m_RCONPort.has_value())
		return false;

	return true;
}

static void OpenTF2(const std::string_view& rconPassword, uint16_t rconPort)
{
	std::string url;
	url << "steam://run/440//"
		" -usercon"
		" +ip 0.0.0.0 +alias ip"
		" +sv_rcon_whitelist_address 127.0.0.1 +alias sv_rcon_whitelist_address"
		" +rcon_password " << rconPassword << " +alias rcon_password"
		" +hostport " << rconPort << " +alias hostport"
		" +net_start"
		" +con_timestamp 1 +alias con_timestamp"
		" -condebug"
		" -conclearlog";

	Shell::OpenURL(std::move(url));
}

TF2CommandLinePage::RCONClientData::RCONClientData(std::string pwd, uint16_t port) :
	m_Client(std::make_unique<srcon::async_client>())
{
	srcon::srcon_addr addr;
	addr.addr = "127.0.0.1";
	addr.pass = std::move(pwd);
	addr.port = port;

	m_Client->set_addr(std::move(addr));

	m_Message = "Connecting...";
}

bool TF2CommandLinePage::RCONClientData::Update()
{
	if (!m_Success)
	{
		if (m_Future.valid() && m_Future.wait_for(0s) == std::future_status::ready)
		{
			try
			{
				m_Message = m_Future.get();
				m_MessageColor = { 0, 1, 0, 1 };
				m_Success = true;
			}
			catch (const srcon::srcon_error& e)
			{
				DebugLog(std::string(__FUNCTION__) << "(): " << e.what());

				using srcon::srcon_errc;
				switch (e.get_errc())
				{
				case srcon_errc::bad_rcon_password:
					m_MessageColor = { 1, 0, 0, 1 };
					m_Message = "Bad rcon password, this should never happen!";
					break;
				case srcon_errc::rcon_connect_failed:
					m_MessageColor = { 1, 1, 0.5, 1 };
					m_Message = "TF2 not yet accepting RCON connections. Retrying...";
					break;
				case srcon_errc::socket_send_failed:
					m_MessageColor = { 1, 1, 0.5, 1 };
					m_Message = "TF2 not yet accepting RCON commands...";
					break;
				default:
					m_MessageColor = { 1, 1, 0, 1 };
					m_Message = "Unexpected error: "s << e.what();
					break;
				}
				m_Future = {};
			}
			catch (const std::exception& e)
			{
				DebugLogWarning(std::string(__FUNCTION__) << "(): " << e.what());
				m_MessageColor = { 1, 0, 0, 1 };
				m_Message = "RCON connection unsuccessful: "s << e.what();
				m_Future = {};
			}
		}

		if (!m_Future.valid())
			m_Future = m_Client->send_command_async("echo RCON connection successful.", false);
	}

	ImGui::TextColoredUnformatted(m_MessageColor, m_Message);

	return m_Success;
}

auto TF2CommandLinePage::OnDraw(const DrawState& ds) -> OnDrawResult
{
	m_Data.TryUpdateCmdlineArgs();

	const auto LaunchTF2Button = [&]
	{
		ImGui::NewLine();
		ImGui::EnabledSwitch(m_Data.m_AtLeastOneUpdateRun, [&]
			{
				if (ImGui::Button("Launch TF2"))
					OpenTF2(m_Data.m_RandomRCONPassword, m_Data.m_RandomRCONPort);

			}, "Finding command line arguments...");
	};

	if (!m_Data.m_CommandLineArgs.has_value())
	{
		m_Data.m_TestRCONClient.reset();
		ImGui::TextUnformatted("TF2 must be launched via TF2 Bot Detector. You can open it by clicking the button below.");
		LaunchTF2Button();
	}
	else if (m_Data.m_MultipleInstances)
	{
		m_Data.m_TestRCONClient.reset();
		ImGui::TextUnformatted("More than one instance of hl2.exe found. Please close the other instances.");

		ImGui::EnabledSwitch(false, LaunchTF2Button, "TF2 is currently running. Please close it first.");
	}
	else if (!m_Data.m_CommandLineArgs.has_value() || !m_Data.m_CommandLineArgs->IsPopulated())
	{
		m_Data.m_TestRCONClient.reset();
		ImGui::TextColoredUnformatted({ 1, 1, 0, 1 }, "Invalid TF2 command line arguments");
		ImGui::NewLine();
		ImGui::TextUnformatted("TF2 must be launched via TF2 Bot Detector. Please close it, then open it again with the button below.");
		ImGui::EnabledSwitch(false, LaunchTF2Button, "TF2 is currently running. Please close it first.");
	}
	else if (!m_Data.m_RCONSuccess)
	{
		auto& args = m_Data.m_CommandLineArgs.value();
		ImGui::TextUnformatted("Connecting to TF2 on 127.0.0.1:"s << args.m_RCONPort.value()
			<< " with password " << std::quoted(args.m_RCONPassword) << "...");

		if (!m_Data.m_TestRCONClient)
			m_Data.m_TestRCONClient.emplace(args.m_RCONPassword, args.m_RCONPort.value());

		ImGui::NewLine();
		m_Data.m_RCONSuccess = m_Data.m_TestRCONClient.value().Update();
	}
	else
	{
		return OnDrawResult::EndDrawing;
	}

	return OnDrawResult::ContinueDrawing;
}

void TF2CommandLinePage::Init(const Settings& settings)
{
	m_Data = {};
	m_Data.m_RandomRCONPassword = GenerateRandomRCONPassword();
	m_Data.m_RandomRCONPort = GenerateRandomRCONPort();
}

void TF2CommandLinePage::Commit(Settings& settings)
{
	settings.m_Unsaved.m_RCONClient = std::move(m_Data.m_TestRCONClient.value().m_Client);
	m_Data.m_TestRCONClient.reset();
}