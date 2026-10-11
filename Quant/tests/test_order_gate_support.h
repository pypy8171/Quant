#pragma once
// test_order_gate 공용 준비 — 신호·교체 설정 도우미와 주제별 시험 묶음 선언.
//  시험 본문은 test_order_gate_<주제>.cpp에 있고, test_order_gate.cpp의 main이 묶음을 차례로 부른다.

#include "risk/OrderGate.h"
#include "ipc/LedgerSnapshot.h"
#include "core/KstTime.h"
#include <algorithm>
#include <cassert>
#include <ctime>
#include <format>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

OrderSignal make_signal(const std::string& ticker, OrderSide side, int quantity = 1);
void PASS(const std::string& name);

// 동시보유 상한 2·교체 켜짐·최소 보유 조건 끔 — 교체 진입·슬롯 제외·장부 사본 시험이 같이 쓴다.
OrderGate::Config displace_config();

// ─── 주제별 시험 묶음 — test_order_gate.cpp의 main이 아래 순서로 부른다 ───────────────
void run_basic_tests();
void run_displace_tests();
void run_session_tests();
void run_ledger_tests();
