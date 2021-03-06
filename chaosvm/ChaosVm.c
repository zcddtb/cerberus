#include "Common.h"
#include "DISKernel.h"
#include "ChaosVm.h"
#include "ChaosVmJmpTargetDataBase.h"
#include "ChaosVmKernel.h"

#pragma comment(linker, "/BASE:0")

/*
 * 注以下文件不得改变位置
 */
#include "ChaosVmDebuger.c"//此文件实现了混乱虚拟机调试器

/*
 * 私有结构
 */
typedef struct _CALL_CONTEXT_REGISTERS {
	__dword dwOrigEAX;
	__dword dwOrigECX;
	__dword dwOrigEDX;
	__dword dwOrigEBX;
	__dword dwOrigESP;
	__dword dwOrigEBP;
	__dword dwOrigESI;
	__dword dwOrigEDI;
	__dword dwOrigEFlag;
} CALL_CONTEXT_REGISTERS, *PCALL_CONTEXT_REGISTERS;

typedef struct _REVERSE_BASIC_REGISTERS {
	__dword dwEFlag;//标志寄存器
	CPU_REGISTER_32 EDI;
	CPU_REGISTER_32 ESI;
	CPU_REGISTER_32 EBP;
	CPU_REGISTER_32 ESP;
	CPU_REGISTER_32 EBX;
	CPU_REGISTER_32 EDX;
	CPU_REGISTER_32 ECX;
	CPU_REGISTER_32 EAX;
} REVERSE_BASIC_REGISTERS, *PREVERSE_BASIC_REGISTERS;

/*
 * 要引出的全局变量
 */
#if defined(__CHAOSVM_MODE_INFECT)
__EXPORT__ CHAOSVM_CONFIGURE g_ChaosVmConfigure = {0};//配置结构
__EXPORT__ PCHAOSVM_RUNTIME g_pChaosVmRuntimeList = NULL;//混乱虚拟机运行环境时列表结构
__EXPORT__ __integer g_iVmpProcedureCount = 0;//记录被保护函数的数量
#else
CHAOSVM_CONFIGURE g_ChaosVmConfigure = {0};//配置结构
PCHAOSVM_RUNTIME g_pChaosVmRuntimeList = NULL;//混乱虚拟机运行环境时列表结构
__integer g_iVmpProcedureCount = 0;//记录被保护函数的数量
#endif

/*
 * 私有的全局变量
 */
CRITICAL_SECTION g_CriticalSection = {0};//很重要的同步对象
PSTACK g_pCallSwitchContextStack = NULL;//CALL函数交换上下文所用的堆栈
PCALL_CONTEXT_REGISTERS g_pCurrCallSwitchContextStack = NULL;//临时所用的递归记录结构指针
__address g_addrNowChaosVmImageBase = 0;//当前的映射基地址
__address g_addrOrigChaosVmImageBase = 0;//原始的映射基地址
__address g_addrNowTargetImageBase = 0;//当前目标基地址
__address g_addrOrigImageBase = 0;//原始的目标文件基地址,主要用于地址随机化后的情况
__integer g_iNowSizeOfImage = 0;//当前目标文件的映射总长度
__integer g_iOrigSizeOfImage = 0;//原始的目标文件的映射总长度
__dword g_dwOrigEAX = 0;
__dword g_dwOrigECX = 0;
__dword g_dwOrigEDX = 0;
__dword g_dwOrigEBX = 0;
__dword g_dwOrigESP = 0;
__dword g_dwOrigEBP = 0;
__dword g_dwOrigESI = 0;
__dword g_dwOrigEDI = 0;
__dword g_dwOrigEFlag = 0;
__dword g_dwRunCalledESP = 0;//保存调用函数后的ESP值

/*
 * 私有宏定义
 */
#define __CHAOSVM_OFFSET_TO_INVOKE_INDEX_FIX__				0x04
#define __CHAOSVM_OFFSET_TO_INVOKE_RANDNUMBER_FIX__			0x09
#define __CHAOSVM_OFFSET_TO_INVOKE_RETADDR_FIX__			0x0E
#define __ReverseBasicRegisters2BasicRegisters__(pReverseBasicRegisters, pBasicRegisters)\
	((PCPU_BASIC_REGISTERS)(pBasicRegisters))->EFlag.BitArray = ((PREVERSE_BASIC_REGISTERS)(pReverseBasicRegisters))->dwEFlag;\
	((PCPU_BASIC_REGISTERS)(pBasicRegisters))->GeneralRegister.Interface.EAX.dwValue32 = ((PREVERSE_BASIC_REGISTERS)(pReverseBasicRegisters))->EAX.dwValue32;\
	((PCPU_BASIC_REGISTERS)(pBasicRegisters))->GeneralRegister.Interface.ECX.dwValue32 = ((PREVERSE_BASIC_REGISTERS)(pReverseBasicRegisters))->ECX.dwValue32;\
	((PCPU_BASIC_REGISTERS)(pBasicRegisters))->GeneralRegister.Interface.EDX.dwValue32 = ((PREVERSE_BASIC_REGISTERS)(pReverseBasicRegisters))->EDX.dwValue32;\
	((PCPU_BASIC_REGISTERS)(pBasicRegisters))->GeneralRegister.Interface.EBX.dwValue32 = ((PREVERSE_BASIC_REGISTERS)(pReverseBasicRegisters))->EBX.dwValue32;\
	((PCPU_BASIC_REGISTERS)(pBasicRegisters))->GeneralRegister.Interface.ESP.dwValue32 = ((PREVERSE_BASIC_REGISTERS)(pReverseBasicRegisters))->ESP.dwValue32;\
	((PCPU_BASIC_REGISTERS)(pBasicRegisters))->GeneralRegister.Interface.EBP.dwValue32 = ((PREVERSE_BASIC_REGISTERS)(pReverseBasicRegisters))->EBP.dwValue32;\
	((PCPU_BASIC_REGISTERS)(pBasicRegisters))->GeneralRegister.Interface.ESI.dwValue32 = ((PREVERSE_BASIC_REGISTERS)(pReverseBasicRegisters))->ESI.dwValue32;\
	((PCPU_BASIC_REGISTERS)(pBasicRegisters))->GeneralRegister.Interface.EDI.dwValue32 = ((PREVERSE_BASIC_REGISTERS)(pReverseBasicRegisters))->EDI.dwValue32;

/*
 * 私有函数声明
 */
typedef __void (__API__ *FPChaosVmInvokeDoor)(__integer iIndex, PREVERSE_BASIC_REGISTERS pReverseBasicRegisters);
__void __API__ ChaosVmInvokeDoor(__integer iIndex, PREVERSE_BASIC_REGISTERS pReverseBasicRegisters);

/*
 * 介绍:
 *	在进入保护函数时首先进入此函数
 *	此段函数会保存当前上下文环境
 *	直接跳到Door函数,返回时直接返回到调用保护函数下一个地址
 *	此函数大小 = ChaosVmInvokeEndStub - ChaosVmInvokeStub
 */
//__NAKED__ __void __API__ ChaosVmInvokeStub() {
//	__asm {
//		;;
//		;; 在虚拟机退出时,恢复到物理环境,堆栈将切换到此刻
//		;; 这时esp刚好指向调用保护函数的返回地址,直接ret即可返回
//		;;
//		pushad																			;1
//		pushfd																			;1
//		push esp;压入一个REVERSE_BASIC_REGISTERS结构									;1
//		push 0x19831210;这里要修改为被保护的函数索引									;5
//		push 0x19831210;随便压入一个值,用作平衡堆栈										;5
//		push 0x19831204;这里需要修改,ChaosVmInvokeDoor的内存地址
//		ret
//	}
//}
//__void __API__ ChaosVmInvokeEndStub() {
//	if (IsDebuggerPresent() == TRUE)
//		ExitProcess(0);
//}

// 默认的调用头
#define __CHAOSVM_DEF_INVOKE_STUB_SIZE__					19
__byte g_DefaultChaosVmInvokeStub[0x20] = {
	"\x60\x9C\x54\x68\x10\x12\x83\x19\x68\x10\x12\x83\x19\x68\x04\x12\x83\x19\xC3"
};
__bool __INTERNAL_FUNC__ ChaosVmGenInvokeAlgorithm(__integer iIndex, __memory pProcAddress, __integer iProcSize, FPChaosVmInvokeDoor pChaosVmInvokeDoor) {
	__memory pInvoke = NULL;
	__integer iInvokeSize = 0;

	//pInvoke = (__memory)ChaosVmInvokeStub;
	//iInvokeSize = (__integer)ChaosVmInvokeEndStub - (__integer)ChaosVmInvokeStub;
	pInvoke = (__memory)g_DefaultChaosVmInvokeStub;
	iInvokeSize = __CHAOSVM_DEF_INVOKE_STUB_SIZE__;
	if (iInvokeSize > iProcSize) return FALSE;

	// 复制调用头,并设置偏移
	__logic_memcpy__(pProcAddress, pInvoke, iInvokeSize);
	*(__dword *)(pProcAddress + __CHAOSVM_OFFSET_TO_INVOKE_INDEX_FIX__) = (__dword)iIndex;
	*(__dword *)(pProcAddress + __CHAOSVM_OFFSET_TO_INVOKE_RANDNUMBER_FIX__) = (__dword)GetRandDword(); 
	*(__dword *)(pProcAddress + __CHAOSVM_OFFSET_TO_INVOKE_RETADDR_FIX__) = (__dword)pChaosVmInvokeDoor;

	return TRUE;
}

/*
 * 参数:
 *	
 * 介绍:
 *	产生函数的调用头
 */
__bool __INTERNAL_FUNC__ ChaosVmGenInvoke(__integer iIndex, PCHAOSVMP_PROCEDURE pVmpProcedure) {
	__memory pProcAddress = NULL;
	__integer iProcSize = 0;
	__bool bRet = FALSE;
	
	pProcAddress = (__memory)(pVmpProcedure->addrProcedureMemoryAddress);
	iProcSize = pVmpProcedure->iSize;

	bRet = ChaosVmGenInvokeAlgorithm(iIndex, pProcAddress, iProcSize, ChaosVmInvokeDoor);

	return bRet;
}

__dword __INTERNAL_FUNC__ ChaosVmHash(__memory pPoint, __integer iSize) {
	return crc32(pPoint, iSize);
}

__integer __INTERNAL_FUNC__ ChaosVmDecrypt(__memory pIn, __integer iSize, __dword dwKey, __memory pOut) {
	__byte *pBuf = __logic_new_size__(iSize + 0x10);
	__byte *pKey = __logic_new_size__(iSize + 0x10);
	__logic_memcpy__(pBuf, pIn, iSize);
	XorKey32Bits(dwKey, pKey, iSize);
	XorCoder(pKey, pBuf, iSize);
	__logic_memcpy__(pOut, pBuf, iSize);//输出
	__logic_delete__(pBuf);
	__logic_delete__(pKey);
	return iSize;
}

__integer __INTERNAL_FUNC__ ChaosVmInstRemainDecrypt(__memory pIn, __integer iSize, __dword dwKey, __memory pOut) {
	__byte *pBuf = __logic_new_size__(iSize + 0x10);
	__byte *pKey = __logic_new_size__(iSize + 0x10);
	__logic_memcpy__(pBuf, pIn, iSize);
	XorKey32Bits(dwKey, pKey, iSize);
	XorCoder(pKey, pBuf, iSize);
	__logic_memcpy__(pOut, pBuf, iSize);//输出
	__logic_delete__(pBuf);
	__logic_delete__(pKey);
	return iSize;
}

/*
 * 参数:
 *	pVmpProcedure:要解密的被保护函数结构
 *	pProcedure:指定要解密的内存空间
 *
 * 介绍:
 *	解压被保护的函数
 */
__memory __INTERNAL_FUNC__ ChaosVmDecryptProcedure(PCHAOSVMP_PROCEDURE pVmpProcedure) {
	// 解压出被保护的代码
	__offset ofVmpProcedureRVA = pVmpProcedure->ofVmpProcedureRVA;
	__offset ofKeyProcedureRVA = pVmpProcedure->ofKeyRVA;
	__memory pProcedure = (__memory)(pVmpProcedure->pVmpProcedure);
	__memory pKeyProcedure = (__memory)(g_addrNowTargetImageBase + ofKeyProcedureRVA);
	__integer iKeySize = pVmpProcedure->iKeySize;
	__integer iSize = pVmpProcedure->iSize;
	__dword dwKey = 0;

	__PrintDbgInfo_DebugerWriteLine__("<chaosvm>Entry ChaosVmDecryptProcedure");

	// 获取解密函数所需的Key
	if (pVmpProcedure->bUseProcKey) {
		dwKey = __GenProcedureKey__(pKeyProcedure, iKeySize);
		pVmpProcedure->dwProcKey = dwKey;

		__PrintDbgInfo_DebugerWriteFormatStringInteger__("<chaosvm>Key procedure address = ", pKeyProcedure);
		__PrintDbgInfo_DebugerWriteFormatStringInteger__("<chaosvm>Key procedure size = ", iKeySize);
		__PrintDbgInfo_DebugerWriteFormatStringInteger__("<chaosvm>Key procedure's Key = ", dwKey);
	} else {
		dwKey = pVmpProcedure->dwProcKey;

		__PrintDbgInfo_DebugerWriteFormatStringInteger__("<chaosvm>Random's Key = ", dwKey);
	}

	// 解密函数
	XorArray(dwKey, pProcedure, pProcedure, iSize);

	return pProcedure;
}

/*
 * 参数:
 *	pCPU:CPU模拟结构
 *	dwValue:要压入的4字节数据
 *
 * 介绍:
 *	压入4字节数据到虚拟机执行的堆栈中
 */
__INLINE__ __void __INTERNAL_FUNC__ ChaosVmCpuPushDword(PCHAOSVM_CPU pCPU, __dword dwValue) {
	__dword *pdwNewEsp = (__dword *)(pCPU->CurrentRegistersStatus.BasicRegister.GeneralRegister.Interface.ESP.dwValue32 - 4);
	*pdwNewEsp = dwValue;
	pCPU->CurrentRegistersStatus.BasicRegister.GeneralRegister.Interface.ESP.dwValue32 = (__dword)pdwNewEsp;
}

/*
 * 参数:
 *	EFlag:标志寄存器
 *	rEax:EAX的值
 *	rEcx:ECX的值
 *	rEdx:EDX的值
 *	rEbx:EBX的值
 *	rEsp:ESP的值
 *	rEbp:EBP的值
 *	rEsi:ESI的值
 *	rEdi:EDI的值
 *
 * 介绍:
 *	将指定的值恢复到物理寄存器中
 */
__NAKED__ __void __INTERNAL_FUNC__ ResumePhysicsContext(__dword EFlag, __dword rEax, __dword rEcx, __dword rEdx, __dword rEbx, \
														__dword rEsp, __dword rEbp, __dword rEsi, __dword rEdi) {
	__asm {
		;; 恢复标志寄存器
		;push EFlag
		popfd
		;; 恢复通用寄存器
		;push rEax
		;push rEcx
		;push rEdx
		;push rEbx
		;push rEsp
		;push rEbp
		;push rEsi
		;push rEdi
		popad
		;add esp, 0x24	;恢复参数堆栈
		ret				; 返回
	}
}

/*
 * 介绍:
 * 此函数作为混乱虚拟机保护的入口函数
 */
#define __DEF_CHAOSVM_RECURRENCE_LAYER_COUNT__					512
#define __DEF_CHAOSVM_STACK_SIZE__								1024 * 8
#define __DEF_CHAOSVM_START_STACK_POINTER__						1024 * 4
#if defined(__CHAOSVM_MODE_EMULATION__)
__INLINE__ __void __INTERNAL_FUNC__ ChaosVmEmulationInit(PCHAOSVM_EMULATION_CONFIGURE pEmulationConfigure) {
	g_addrNowTargetImageBase = pEmulationConfigure->dwTargetNowImageBase;
	g_addrOrigImageBase = pEmulationConfigure->dwTargetOrigImageBase;
	g_iNowSizeOfImage = pEmulationConfigure->dwTargetNowSizeOfImage;
	g_iOrigSizeOfImage = pEmulationConfigure->dwTargetOrigSizeOfImage;
}

__void __API__ ChaosVmInit(PCHAOSVM_EMULATION_CONFIGURE pEmulationConfigure, PCHAOSVM_EMULATION_BYTECODE_FILE pByteCodeFile, PCHAOSVM_RUNTIME pRuntime)
#else
__void __API__ ChaosVmInit()
#endif
{
	__integer i = 0;
	__address addrPrevProcedure = NULL;
	__integer iPrevProcedureSize = 0;

#if !defined(__CHAOSVM_MODE_EMULATION__)
	// 在没有定义虚拟机仿真模式下的局部变量
	PCHAOSVM_EMULATION_BYTECODE_FILE pByteCodeFile = NULL;
#endif

	// 断点
	__PrintDbgInfo_DebugerInit__(ChaosVmDebuger);
	__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Entry ChaosVmInit", NULL);

	// 初始化一些所用的数据
#if !defined(__CHAOSVM_MODE_EMULATION__)
	g_pChaosVmRuntimeList = (PCHAOSVM_RUNTIME)(g_addrNowTargetImageBase + (__address)g_pChaosVmRuntimeList);

	// 判断是否使用字节码文件
	if (g_ChaosVmConfigure.bUseByteCodeFile) {
		__memory pByteCodeMem = NULL;
		__integer iByteCodeSize = 0;
		__tchar szByteCodeFilePath[MAX_PATH] = {0};
		GetLocalPath(NULL, szByteCodeFilePath);//在被保护程序的目录下
		__logic_tcscat__(szByteCodeFilePath, g_ChaosVmConfigure.szChaosVmByteCodeFileName);
		pByteCodeMem = MappingFile(szByteCodeFilePath, &iByteCodeSize, FALSE, 0, 0);
		if (!pByteCodeMem) {
			// 字节码文件不存在

			if (__logic_tcslen__(g_ChaosVmConfigure.szMessageBoxTitle) == 0)
				__logic_tcscpy__(g_ChaosVmConfigure.szMessageBoxTitle, _T("ChaosVm"));

			if (__logic_tcslen__(g_ChaosVmConfigure.szMessageBoxContext) == 0)
				__logic_tcscpy__(g_ChaosVmConfigure.szMessageBoxContext, _T("The chaosvm byte code file not exist"));

			MessageBox(NULL, g_ChaosVmConfigure.szMessageBoxContext, g_ChaosVmConfigure.szMessageBoxTitle, g_ChaosVmConfigure.dwMessageStyle);
			ExitProcess(-1);
		}

		// 分配储存空间
		(__memory)pByteCodeFile = (__memory)__logic_new_size__(iByteCodeSize);
		if (!pByteCodeFile) {
			// 分配内存失败
		}
		__logic_memcpy__(pByteCodeFile, pByteCodeMem, iByteCodeSize);
		UnMappingFile(pByteCodeMem);

		// 验证字节码文件
		{
			__dword dwByteCodeFileCrc32 = 0;
			if (!ChaosVmByteCodeFileVerifySign(pByteCodeFile, &dwByteCodeFileCrc32)) {
				// 字节码文件的签名值错误
				__PrintDbgInfo_DebugerWriteLine__("<chaosvm>ChaosVm byte code file verify sign failed");
				__logic_delete__(pByteCodeFile);
				ExitProcess(-1);
			}/* end if */
			__PrintDbgInfo_DebugerWriteFormatStringInteger__("<chaosvm>ChaosVm byte code file sign = ", dwByteCodeFileCrc32);
		}
	}
#else
	ChaosVmEmulationInit(pEmulationConfigure);
	g_pChaosVmRuntimeList = pRuntime;
	g_iVmpProcedureCount = ChaosVmByteCodeFileGetProcedureCount(pByteCodeFile);
#endif

	// 解密函数,并生成调用头
	__PrintDbgInfo_DebugerWriteFormatStringInteger__("<chaosvm>All of procedure will be init,cout = ", g_iVmpProcedureCount);
	for (i = 0; i < g_iVmpProcedureCount; i++) {
		PCHAOSVM_RUNTIME pRuntime = &(g_pChaosVmRuntimeList[i]);
		PCHAOSVMP_PROCEDURE pChaosVmpProcedure = &(pRuntime->Procedure);
		PCHAOSVM_CPU pCPU = &(pRuntime->Cpu);
		__integer iIndex = pRuntime->iIndex;

		// 设置函数当前的地址
#if defined(__CHAOSVM_MODE_EMULATION__)
		pChaosVmpProcedure->addrProcedureMemoryAddress = g_addrNowTargetImageBase + pChaosVmpProcedure->ofProcedureMemoryAddress;//被保护前的地址
		pChaosVmpProcedure->pVmpProcedure = ChaosVmByteCodeFileGetProcedureByteCode(i, pByteCodeFile);//被保护后的地址
#else
		pChaosVmpProcedure->addrProcedureMemoryAddress = g_addrNowTargetImageBase + pChaosVmpProcedure->ofProcedureMemoryAddress;//被保护前的地址
		// 如果要使用字节码文件
		if (g_ChaosVmConfigure.bUseByteCodeFile)
			pChaosVmpProcedure->pVmpProcedure = ChaosVmByteCodeFileGetProcedureByteCode(i, pByteCodeFile);//被保护后的地址
		else
			pChaosVmpProcedure->pVmpProcedure = (__memory)(g_addrNowTargetImageBase + pChaosVmpProcedure->ofVmpProcedureRVA);//被保护后的地址
#endif

		// 打印当前被保护函数的地址
		__PrintDbgInfo_DebugerWriteFormatStringInteger__("<chaosvm>Procedure address = ", pChaosVmpProcedure->pVmpProcedure);

		// 解密函数
		__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Already decrypt procedure", pChaosVmpProcedure);
		pChaosVmpProcedure->pVmpProcedure = ChaosVmDecryptProcedure(pChaosVmpProcedure);//解密

		// 生成调用头
		__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Already generate invoke stub procedure", pChaosVmpProcedure);
		if (ChaosVmGenInvoke(iIndex, pChaosVmpProcedure)) {
			__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Generate invoke stub procedure success", pChaosVmpProcedure);
		} else {
			__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Generate invoke stub procedure failed", pChaosVmpProcedure);
		}

		// 分配此函数所需的堆栈
		pRuntime->iStackSize = __DEF_CHAOSVM_STACK_SIZE__;//8KB堆栈空间,上下4KB空间
		pRuntime->pStack = (__memory)__logic_new_size__(pRuntime->iStackSize);//混乱虚拟机运行堆栈
		if (!(pRuntime->pStack)) {
			__PrintDbgInfo_DebugerWriteLine__("<chaosvm>Alloc memory for stack failed");
			return;
		}

		// 进行上一条地址的记录
		pRuntime->addrPrevProcedureAddress = addrPrevProcedure;
		pRuntime->iPrevProcedureSize = iPrevProcedureSize;
		__PrintDbgInfo_DebugerWriteFormatStringInteger__("<chaosvm>Prev procedure address = ", addrPrevProcedure);
		__PrintDbgInfo_DebugerWriteFormatStringInteger__("<chaosvm>Prev procedure size = ", iPrevProcedureSize);

		// 更新上一条函数地址
		addrPrevProcedure = (__address)(pChaosVmpProcedure->pVmpProcedure);
		iPrevProcedureSize = pChaosVmpProcedure->iSize;

		// 我是华丽的分割线
		__PrintDbgInfo_DebugerWriteLine__("<chaosvm>----------------------------------------");
	}/* end for */

	// 初始化全局同步变量
	InitializeCriticalSection(&g_CriticalSection);

	// 初始化CALL交换堆栈环
	g_pCallSwitchContextStack = init_stack(__DEF_CHAOSVM_RECURRENCE_LAYER_COUNT__ * sizeof(PCALL_CONTEXT_REGISTERS) ,TRUE);
	if (!g_pCallSwitchContextStack) {
		__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>init_stack for CallContextRegisters failed", NULL);
	}
	//else {
	//	__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Already goto OrigEntryAddress", NULL);
	//	// 跳入到原始入口点
	//	__asm {
	//		push g_addrOrigEntryAddress
	//		ret
	//	}
	//}/* end else */

	// 在感染模式下才起作用
#if defined(__CHAOSVM_MODE_INFECT__)
	//////////////////////////////////////////////////////////////////////////
	// 力量保护器配置
	{
		POWER_PROTECTER_INFO PowerProtecterInfo = {0};
		PPOWER_PROTECTER_CONFIGURE pPowerProtecterConfigure = NULL;

		__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Configure PowerProtecter", NULL);

		// 设置力量保护器信息
		PowerProtecterInfo.addrNowTargetImageBase = g_addrNowTargetImageBase;
		PowerProtecterInfo.addrOrigTargetImageBase = g_addrOrigImageBase;
		PowerProtecterInfo.addrPowerProtecterNowImageBase = g_addrNowChaosVmImageBase;
		PowerProtecterInfo.addrPowerProtecterOrigImageBase = g_addrOrigChaosVmImageBase;

		pPowerProtecterConfigure = &(g_ChaosVmConfigure.PowerProtecterConfigure);
		PowerProtecterInit(pPowerProtecterConfigure, &PowerProtecterInfo);
	}
#endif

}

/*
 * 介绍:
 *	负责初始化
 */
#if defined(__CHAOSVM_MODE_EMULATION__)
__void __API__ ChaosVmEntry(PCHAOSVM_EMULATION_CONFIGURE pConfigure, PCHAOSVM_EMULATION_BYTECODE_FILE pByteCodeFile, PCHAOSVM_RUNTIME pRuntime) {
#if defined(_DEBUG)
	//__asm int 3;
#endif
	//////////////////////////////////////////////////////////////////////////
	// 调试功能 2012.2.9 新增
	if (pConfigure->DebugConfigure.bDebugChaosVm == TRUE)
	{
		if (pConfigure->DebugConfigure.bBreakPoint == TRUE) {
			__asm int 3;
		} else {
			while (pConfigure->DebugConfigure.bDebugChaosVm == TRUE) {
				Sleep(1000);
			}
		}/* end else */
	}

	ChaosVmInit(pConfigure, pByteCodeFile, pRuntime);
}
#else
__void __API__ ChaosVmEntry() {
#if defined(_DEBUG)
	__asm int 3;
#endif
	// 初始化混乱虚拟机
	ChaosVmInit();
}
#endif

/*
 * 参数:
 *	pChaosVmRuntime:混乱虚拟机运行环境时
 *	addrCallProc:CALL函数的地址
 *
 * 介绍:
 *	这个函数是模拟混乱虚拟机在虚拟执行过程中的CALL指令
 *	这里的切栈是一个复杂的过程,有三点需要考虑,一点是虚拟机运行的堆栈与虚拟机CPU堆栈之间的切换
 *	一点是多线程兼容的考虑,多线程使用同步对象完成,第三点是关于虚拟机调用虚拟机,虚拟机又调用其他
 *	外部函数或者再次进入其他虚拟机的递归调用
 */
__void __INTERNAL_FUNC__ ChaosVmCall(PCHAOSVM_RUNTIME pChaosVmRuntime, __address addrCallProc) {
	PCPU_BASIC_REGISTERS pBasicRegister = &(pChaosVmRuntime->Cpu.CurrentRegistersStatus.BasicRegister);
	PCPU_BASIC_REGISTERS pLastCheckPoint = &(pChaosVmRuntime->LastCheckPoint);
	PCALL_CONTEXT_REGISTERS pCallContextRegisters = NULL;
	__integer iSizeOfCpuBasicRegisters = sizeof(CPU_BASIC_REGISTERS);
	__dword dwEFlag = 0, dwEAX = 0, dwECX = 0, dwEDX = 0, dwEBX = 0, dwESP = 0, dwEBP = 0, dwESI = 0, dwEDI = 0;
	__address addrRetAddressPointer = 0;
	// 保存最后一次物理寄存器状态
	__asm {
		pushfd
		pop dwEFlag
		pushad
		pop dwEDI
		pop dwESI
		pop dwEBP
		pop dwESP
		pop dwEBX
		pop dwEDX
		pop dwECX
		pop dwEAX
	}
	pLastCheckPoint->EFlag.BitArray = dwEFlag;
	pLastCheckPoint->GeneralRegister.Interface.EAX.dwValue32 = dwEAX;
	pLastCheckPoint->GeneralRegister.Interface.ECX.dwValue32 = dwECX;
	pLastCheckPoint->GeneralRegister.Interface.EDX.dwValue32 = dwEDX;
	pLastCheckPoint->GeneralRegister.Interface.EBX.dwValue32 = dwEBX;
	pLastCheckPoint->GeneralRegister.Interface.ESP.dwValue32 = dwESP;
	pLastCheckPoint->GeneralRegister.Interface.EBP.dwValue32 = dwEBP;
	pLastCheckPoint->GeneralRegister.Interface.ESI.dwValue32 = dwESI;
	pLastCheckPoint->GeneralRegister.Interface.EDI.dwValue32 = dwEDI;

	// 给递归调用记录结构分配空间
	pCallContextRegisters = __logic_new_size__(sizeof(CALL_CONTEXT_REGISTERS));

	// 获取锁
	EnterCriticalSection(&g_CriticalSection);

	// 将地址压入堆栈
	push_stack(g_pCallSwitchContextStack, (__void *)&pCallContextRegisters, sizeof(PCALL_CONTEXT_REGISTERS));

	// 保存最后状态到全局变量
	pCallContextRegisters->dwOrigEFlag = dwEFlag;
	pCallContextRegisters->dwOrigEAX = dwEAX;
	pCallContextRegisters->dwOrigECX = dwECX;
	pCallContextRegisters->dwOrigEDX = dwEDX;
	pCallContextRegisters->dwOrigEBX = dwEBX;
	pCallContextRegisters->dwOrigESP = dwESP;
	pCallContextRegisters->dwOrigEBP = dwEBP;
	pCallContextRegisters->dwOrigESI = dwESI;
	pCallContextRegisters->dwOrigEDI = dwEDI;

	// 恢复物理环境
	dwEFlag = pBasicRegister->EFlag.BitArray;
	dwEAX = pBasicRegister->GeneralRegister.Interface.EAX.dwValue32;
	dwECX = pBasicRegister->GeneralRegister.Interface.ECX.dwValue32;
	dwEDX = pBasicRegister->GeneralRegister.Interface.EDX.dwValue32;
	dwEBX = pBasicRegister->GeneralRegister.Interface.EBX.dwValue32;
	dwESP = pBasicRegister->GeneralRegister.Interface.ESP.dwValue32;
	dwEBP = pBasicRegister->GeneralRegister.Interface.EBP.dwValue32;
	dwESI = pBasicRegister->GeneralRegister.Interface.ESI.dwValue32;
	dwEDI = pBasicRegister->GeneralRegister.Interface.EDI.dwValue32;

	// 构建运行的堆栈
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), (__dword)0);//将要调用的地址首先压栈,这里为占位
	//
	// 执行完以上指令后,pBasicRegister->GeneralRegister.Interface.ESP.dwValue32中的值即指向以上压入后的地址
	//
	addrRetAddressPointer = (__address)pBasicRegister->GeneralRegister.Interface.ESP.dwValue32;//保存当前的ESP地址
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), (__dword)addrCallProc);//将要调用的地址首先压栈
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwEFlag);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwEAX);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwECX);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwEDX);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwEBX);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwESP);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwEBP);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwESI);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwEDI);
	dwESP = pBasicRegister->GeneralRegister.Interface.ESP.dwValue32;
	__asm {
		;; 获取返回地址
		mov eax, _ret_address										; 获取当返回地址
		mov ebx, addrRetAddressPointer
		mov dword ptr [ebx], eax									; 设置返回地址	
		mov esp, dwESP												; 切栈操作
		popad														; 这里恢复物理寄存器
		popfd														; 恢复存在于当前栈顶的标志寄存器
		ret															; 栈顶的地址为要调用的地址
_ret_address:
	}
	
	/*
	 * 将保存的记录结构弹出堆栈
	 * 在此之前保存标志寄存器与当前寄存器的值
	 * 在调用pop_stack后恢复
	 */
	__asm {//保存运行后的上下文
		pushad
		pushfd
	}

	// 弹出递归记录结构
	(__address)g_pCurrCallSwitchContextStack = *(__address *)pop_stack(g_pCallSwitchContextStack, sizeof(PCALL_CONTEXT_REGISTERS));

	/*
	 * 这里都是公共变量操作不受本地堆栈的影响
	 */
	g_dwOrigEAX = g_pCurrCallSwitchContextStack->dwOrigEAX;
	g_dwOrigECX = g_pCurrCallSwitchContextStack->dwOrigECX;
	g_dwOrigEDX = g_pCurrCallSwitchContextStack->dwOrigEDX;
	g_dwOrigEBX = g_pCurrCallSwitchContextStack->dwOrigEBX;
	g_dwOrigESP = g_pCurrCallSwitchContextStack->dwOrigESP;
	g_dwOrigEBP = g_pCurrCallSwitchContextStack->dwOrigEBP;
	g_dwOrigESI = g_pCurrCallSwitchContextStack->dwOrigESI;
	g_dwOrigEDI = g_pCurrCallSwitchContextStack->dwOrigEDI;
	g_dwOrigEFlag = g_pCurrCallSwitchContextStack->dwOrigEFlag;

	/*
	 * 将此时的物理环境设置到虚拟CPU环境中
	 * 保存完毕后进行最后一次记录点还原工作
	 */
	__asm {
		mov g_dwRunCalledESP, esp									; 将运行完函数后的ESP记录
		;; 恢复到虚拟机CPU运行环境
		mov eax, g_dwOrigEAX
		mov ecx, g_dwOrigECX
		mov edx, g_dwOrigEDX
		mov ebx, g_dwOrigEBX
		mov esp, g_dwOrigESP										; 这里完成切栈
		mov ebp, g_dwOrigEBP
		mov esi, g_dwOrigESI
		mov edi, g_dwOrigEDI
		push g_dwOrigEFlag
		popfd
	}

	// 从g_dwRunCalledESP中取出函数返回的值
	__ReverseBasicRegisters2BasicRegisters__(g_dwRunCalledESP, pBasicRegister);
	// 释放递归记录结构内存
	__logic_delete__(g_pCurrCallSwitchContextStack);
	// 释放全局同步对象
	LeaveCriticalSection(&g_CriticalSection);
}

/*
 * 参数:
 *	pRuntime:运行环境时
 * addrVisAddress:混乱虚拟地址
 *
 * 介绍:
 *	将在混乱虚拟机中的虚拟地址转换为真实环境中的物理地址
 *
 *	暂时此函数没有任何地方进行引用
 */
__INLINE__ __address __INTERNAL_FUNC__ ChaosVmSwitchVis2RelBase(PCHAOSVM_RUNTIME pRuntime, __address addrVisAddress) {
	PCHAOSVMP_PROCEDURE pProcedure = &(pRuntime->Procedure);
	__address addrRelAddress = 0;
	if ((addrVisAddress >= (__address)(pProcedure->pVmpProcedure)) && \
		(addrVisAddress < (__address)(pProcedure->pVmpProcedure) + (__address)(pProcedure->iSize)))
		addrRelAddress = addrVisAddress - (__address)(pProcedure->pVmpProcedure) + pProcedure->addrProcedureMemoryAddress;
	else {
		addrRelAddress = addrVisAddress;
	}
	return addrRelAddress;
}

/*
 * 参数:
 *	pRuntime:运行环境时
 *	addrRelAddress:真实的地址
 *
 * 介绍:
 *	将物理地址转换为虚拟环境中的虚拟地址
 */
__INLINE__ __address __INTERNAL_FUNC__ ChaosVmSwitchRel2VisBase(PCHAOSVM_RUNTIME pRuntime, __address addrRelAddress) {
	PCHAOSVMP_PROCEDURE pProcedure = &(pRuntime->Procedure);
	__address addrVisAddress = 0;
	if ((addrRelAddress >= pProcedure->addrProcedureMemoryAddress) && \
		(addrRelAddress < pProcedure->addrProcedureMemoryAddress + pProcedure->iSize))
		addrVisAddress = addrRelAddress - pProcedure->addrProcedureMemoryAddress + (__address)(pProcedure->pVmpProcedure);
	else {
		/*
		 * 这里如果给予的真实地址不在当前映射范围内
		 * 则表明可能是堆或者栈的内存,直接返回
		 */
		if ((addrRelAddress >= g_addrOrigImageBase) && \
			(addrRelAddress < g_addrOrigImageBase + g_iNowSizeOfImage)) {
			addrVisAddress = addrRelAddress;
			addrVisAddress -= g_addrOrigImageBase;
			addrVisAddress += g_addrNowTargetImageBase;
		} else {
			// 堆或者栈的地址,直接返回
			addrVisAddress = addrRelAddress;
		}/* end else */
	}
	return addrVisAddress;
}

/*
 * 参数:
 *	pCPU:虚拟CPU结构
 * pUserContext:CPU自定义的上下文结构
 * wSeg:段寄存器
 * addrAddress:真实的物理地址
 * bSizeToReadByte:要读取的字节数
 * pOutputBuffer:读取后的缓存
 * pSizeReturnedByte:读取的实际大小
 *
 * 介绍:
 *	在虚拟CPU遇到读取指令时调用,将读取的指令输出,这个函数有一个重要的操作就是解密当前运行的指令
 */
CPU_STATUS __INTERNAL_FUNC__ ChaosVmMmCodeRead(PCHAOSVM_CPU pCPU, __void *pUserContext, __word wSeg, __address addrAddress, __byte bSizeToReadByte, \
												   __byte *pOutputBuffer, __byte *pSizeReturnedByte) {
	PCHAOSVM_RUNTIME pRuntime = (PCHAOSVM_RUNTIME)pUserContext;
	__memory pReadPoint = NULL;
	PCHAOSVMP_JMPTARGET_INST pTargetJmpInst = NULL;
	__dword dwKey = 0;
	
	// 检验是否是跳转目标地址,如果是跳转目标地址则做特殊处理
	pTargetJmpInst = QueryJmpTargetInstFromTable(pRuntime->iIndex, addrAddress);
	if (pTargetJmpInst) {//是跳转目标地址
		// 如果是函数头, 则使用初始化Key
		if (pTargetJmpInst->iPrevInstRandLen == 0) {
			dwKey = __GetInitKey__(pCPU);
		} else {
			pReadPoint = ChaosVmSwitchRel2VisBase(pRuntime, addrAddress);
			dwKey = ChaosVmHash(pReadPoint - pTargetJmpInst->iPrevInstRandLen, pTargetJmpInst->iPrevInstRandLen);//使用上一条指令的部分密文计算密钥
			__try {
				PCHAOSVMP_PROCEDURE pProcedure = &(pRuntime->Procedure);
				__integer iEncryptLength = 0;
				__memory pOpcodeBuf = pOutputBuffer;
				CHAOSVMP_INSTRUCTION Instruction;
				__logic_memcpy__(pOpcodeBuf, pReadPoint, bSizeToReadByte);
				ChaosVmDecrypt((__memory)&(pProcedure->ProtectInstruction[pTargetJmpInst->iInstIndex]), sizeof(CHAOSVMP_INSTRUCTION), dwKey, &Instruction);
				iEncryptLength = Instruction.iInstEncryptLength;
				ChaosVmDecrypt(pOpcodeBuf, iEncryptLength, dwKey, pOpcodeBuf);//解密指令部分
				dwKey = ChaosVmHash(pOpcodeBuf, iEncryptLength);//使用当前指令的指令的明文计算下一条指令的KEY
				*pSizeReturnedByte = bSizeToReadByte;
				pRuntime->iInstIndex = pTargetJmpInst->iInstIndex;//重新设置当前的指令索引
			} __except(__EXCEPTION_EXECUTE_HANDLER__) {
				*pSizeReturnedByte = 0;
				return __CPU_STATUS_MM_INVALID_ADDRESS__;
			}
		}
	} else {//不是跳转目标地址
		pReadPoint = (__memory)ChaosVmSwitchRel2VisBase(pRuntime, addrAddress);
		__try {
			PCHAOSVMP_PROCEDURE pProcedure = &(pRuntime->Procedure);
			__integer iEncryptLength = 0;
			__memory pOpcodeBuf = pOutputBuffer;
			CHAOSVMP_INSTRUCTION Instruction;
			dwKey = pRuntime->dwKey;
			__logic_memcpy__(pOpcodeBuf, pReadPoint, bSizeToReadByte);
			ChaosVmDecrypt((__memory)&(pProcedure->ProtectInstruction[pRuntime->iInstIndex]), sizeof(CHAOSVMP_INSTRUCTION), dwKey, &Instruction);
			iEncryptLength = Instruction.iInstEncryptLength;
			ChaosVmDecrypt(pOpcodeBuf, iEncryptLength, dwKey, pOpcodeBuf);//解密指令部分
			dwKey = ChaosVmHash(pOpcodeBuf, iEncryptLength);//使用当前指令的指令的明文计算下一条指令的KEY
			*pSizeReturnedByte = bSizeToReadByte;
		} __except(__EXCEPTION_EXECUTE_HANDLER__) {
			*pSizeReturnedByte = 0;
			return __CPU_STATUS_MM_INVALID_ADDRESS__;
		}
	}

	pRuntime->dwKey = dwKey;//更新密钥
	return __CPU_STATUS_MM_ACCESS_SUCCESS__;
}

/*
 * 参数:
 *	pCPU:虚拟CPU结构
 * pUserContext:CPU自定义的上下文结构
 * wSeg:段寄存器
 * addrAddress:真实的物理地址
 * bSizeToReadByte:要读取的字节数
 * pOutputBuffer:读取后的缓存
 * pSizeReturnedByte:读取的实际大小
 *
 * 介绍:
 *	在虚拟CPU遇到读取数据时调用
 */
CPU_STATUS __INTERNAL_FUNC__ ChaosVmMmRead(PCHAOSVM_CPU pCPU, __void *pUserContext, __word wSeg, __address addrAddress, __byte bSizeToReadByte, \
										   __byte *pOutputBuffer, __byte *pSizeReturnedByte) {
	PCHAOSVM_RUNTIME pRuntime = (PCHAOSVM_RUNTIME)pUserContext;
	__memory pReadPoint = (__memory)ChaosVmSwitchRel2VisBase(pRuntime, addrAddress);

	__try {
		// 对于FS寄存器的处理
		if (pCPU->bDataSegmentRegister == CHAOSVM_SR_FS) {
			__offset ofs = (__offset)addrAddress;

			if (bSizeToReadByte == sizeof(__byte)) {
				*(__byte *)pOutputBuffer = __readfsbyte(ofs);
			} else if (bSizeToReadByte == sizeof(__word)) {
				*(__word *)pOutputBuffer = __readfsword(ofs);
			} else if (bSizeToReadByte == sizeof(__dword)) {
				*(__dword *)pOutputBuffer = __readfsdword(ofs);
			} else if (bSizeToReadByte == sizeof(__qword)) {
				//*(__qword *)pOutputBuffer = __readfsqword(ofs);
			} else {
				// 长度错误
			}
		} else {
			__logic_memcpy__(pOutputBuffer, pReadPoint, bSizeToReadByte);
		}
		
	   *pSizeReturnedByte = bSizeToReadByte;
	} __except(__EXCEPTION_EXECUTE_HANDLER__) {
	   *pSizeReturnedByte = 0;
	   return __CPU_STATUS_MM_INVALID_ADDRESS__;
	}
	return __CPU_STATUS_MM_ACCESS_SUCCESS__;
}

/*
 * 参数:
 *	pCPU:虚拟CPU结构
 * pUserContext:CPU自定义的上下文结构
 * wSeg:段寄存器
 * addrAddress:真实的物理地址
 * bSizeToWriteByte:要写入的字节数
 * pInputBuffer:读取的指针
 *
 * 介绍:
 *	在虚拟CPU遇到写入数据时调用
 */
CPU_STATUS __INTERNAL_FUNC__ ChaosVmMmWrite(PCHAOSVM_CPU pCPU, __void *pUserContext, __word wSeg, __address addrAddress, __byte bSizeToWriteByte, \
											__byte *pInputBuffer) {
	PCHAOSVM_RUNTIME pRuntime = NULL;
	__memory pWritePoint = NULL;
	pRuntime = (PCHAOSVM_RUNTIME)pUserContext;
	pWritePoint = (__memory)ChaosVmSwitchRel2VisBase(pRuntime, addrAddress);
	__try {
		// 对于fs寄存器
		if (pCPU->bDataSegmentRegister == CHAOSVM_SR_FS) {
			__offset ofs = (__offset)addrAddress;

			if (bSizeToWriteByte == sizeof(__byte)) {
				 __writefsbyte(ofs, *(__byte *)pInputBuffer);
			} else if (bSizeToWriteByte == sizeof(__word)) {
				 __writefsword(ofs, *(__word *)pInputBuffer);
			} else if (bSizeToWriteByte == sizeof(__dword)) {
				 __writefsdword(ofs, *(__dword *)pInputBuffer);
			} else if (bSizeToWriteByte == sizeof(__qword)) {
				//__writefsqword(ofs, *(__qword *)pOutputBuffer);
			} else {
				// 长度错误
			}
		} else {
			__logic_memcpy__(pWritePoint, pInputBuffer, bSizeToWriteByte);
		}
	} __except(__EXCEPTION_EXECUTE_HANDLER__) {
		return __CPU_STATUS_MM_INVALID_ADDRESS__;
	}
	return __CPU_STATUS_MM_ACCESS_SUCCESS__;
}

/*
 * 参数:
 *	pCPU:虚拟CPU结构
 * pUserContext:CPU自定义的上下文结构
 * bImmIndexInOpcodeBuffer:立即数离指令头的偏移
 * bSizeToReadByte:读取数据的长度
 *
 * 介绍:
 *	读取指令的数据部分,如果一个指令存在数据部分,那么此函数用于解密此部分数据
 */
__dword __INTERNAL_FUNC__ ChaosVmMmReadDataPart(PCHAOSVM_CPU pCPU, __void *pUserContext, __byte bImmIndexInOpcodeBuffer, __byte bSizeToReadByte) {
	/*
	 * 获取Opcode明文,以及当前Opcode长度计算出解密KEY
	 * 如果当前指令存在偏移则要将偏移的长度考虑进去
	 * 偏移的读取一定在数据读取之前进行
	 */
	PCHAOSVM_RUNTIME pRuntime = (PCHAOSVM_RUNTIME)(pUserContext);
	__integer iDispSize = pRuntime->iDispSize;
	__dword dwRet = 0;
	__byte *pOpcodeBuf = pCPU->OpcodeBuffer;//已解密的指令部分opcode
	__dword dwKey = __GetKey__(pCPU);
	if (iDispSize) {
		__byte bOrigBuf[0x10] = {0};
		__byte bDeBuf[0x10] = {0};
		__memory pDataPart = NULL;
		__logic_memcpy__(bOrigBuf, pOpcodeBuf + bImmIndexInOpcodeBuffer - iDispSize, iDispSize + bSizeToReadByte);
		ChaosVmInstRemainDecrypt(bOrigBuf, iDispSize + bSizeToReadByte, dwKey, bDeBuf);
		pDataPart = bDeBuf + iDispSize;
		if (bSizeToReadByte == sizeof(__byte))
			(__byte)dwRet = *(__byte *)pDataPart;
		else if (bSizeToReadByte == sizeof(__word))
			(__word)dwRet = *(__word *)pDataPart;
		else //if (bSizeToReadByte == sizeof(__dword))
			(__dword)dwRet = *(__dword *)pDataPart;
	} else {//无偏移的情况
		ChaosVmInstRemainDecrypt(pOpcodeBuf + bImmIndexInOpcodeBuffer, bSizeToReadByte, dwKey, (__memory)&dwRet);
	}

	/*
	 * 在这里判断此立即数是否原先是地址
	 * 如果此值的范围在映射范围之内则表示是一个地址
	 * 这种判断有些武断了
	 */
	if ((dwRet >= g_addrOrigImageBase) && (dwRet < g_addrOrigImageBase + g_iNowSizeOfImage)) {
		dwRet -= g_addrOrigImageBase;
		dwRet += g_addrNowTargetImageBase;
	}
	return dwRet;
}

/*
 * 参数:
 *	pCPU:虚拟CPU结构
 * pUserContext:CPU自定义的上下文结构
 * pDispStart:要读取偏移的指针
 * bSizeToReadByte:读取数据的长度
 *
 * 介绍:
 *	读取指令的数据部分,如果一个指令存在数据部分,那么此函数用于解密此部分数据
 */
__dword __INTERNAL_FUNC__ ChaosVmMmReadDispPart(PCHAOSVM_CPU pCPU, __void *pUserContext, __memory pDispStart, __byte bSizeToReadByte) {
	/*
	 * 获取Opcode明文,以及当前Opcode长度计算出解密KEY
	 */
	PCHAOSVM_RUNTIME pRuntime = (PCHAOSVM_RUNTIME)(pUserContext);
	__dword dwRet = 0;
	__dword dwKey = __GetKey__(pCPU);
	ChaosVmInstRemainDecrypt(pDispStart, bSizeToReadByte, dwKey, (__memory)&dwRet);
	pRuntime->iDispSize = bSizeToReadByte;
	return dwRet;
}

/* 
 * 参数:
 *	pCPU:虚拟CPU结构
 *	pUserContext:自定义内容结构
 *
 * 介绍:
 *	处理异常
 *	暂时不做任何动作
 */
CPU_STATUS __INTERNAL_FUNC__ ChaosVmCpuInterruptHandle(PCHAOSVM_CPU pCPU, __void *pUserContext) {
	return __CPU_STATUS_EXECUTE_SUCCESS__;
}

/* 
 * 参数:
 *	pCPU:虚拟CPU结构
 *	pUserContext:自定义内容结构,当前设定为当前被保护函数的运行环境时
 *	wNewSeg:CALL后新的段寄存器
 *	addrNewAddress:CALL后的地址
 *	bOpcodeLength:OP的长度
 *
 * 介绍:
 *	虚拟执行中的CALL指令,如果CALL是在保护函数内(C/C++,PASCAL等语言不支持函数嵌套,所以这种情况一般不会发生)则直接停止虚拟运行退出模拟
 *	如果是在保护函数外则调用ChaosVmCall
 */
CPU_STATUS __INTERNAL_FUNC__ ChaosVmCpuCallFlowControl(PCHAOSVM_CPU pCPU, __void *pUserContext, __word wNewSeg, __address addrNewAddress, __byte bOpcodeLength) {
	PCHAOSVM_RUNTIME pRuntime = (PCHAOSVM_RUNTIME)(pUserContext);
	PCHAOSVMP_PROCEDURE pProcedure = &(pRuntime->Procedure);

	/*
	 * 如果是跳转到保护函数范围之外
	 */
	if ((addrNewAddress < pProcedure->addrProcedureMemoryAddress) || \
		(addrNewAddress >= pProcedure->addrProcedureMemoryAddress + pProcedure->iSize)) {
			ChaosVmCall(pRuntime, addrNewAddress);
			return __CPU_STATUS_HOOK_HANDLE__;
	}

	/*
	 * 在保护范围内,CALL指令不支持,原因是RET指令不支持
	 * RET指令无法核算JmpTarget
	 * 退出模拟
	 */
	return __CPU_STATUS_HOOK_STOP_CPU__;
}

/* 
 * 参数:
 *	pCPU:虚拟CPU结构
 *	pUserContext:自定义内容结构,当前设定为当前被保护函数的运行环境时
 *	wNewSeg:CALL后新的段寄存器
 *	addrNewAddress:CALL后的地址
 *	bOpcodeLength:OP的长度
 *
 * 介绍:
 *	虚拟执行中的RET指令,如果RET后的地址是在保护函数内(C/C++,PASCAL等语言不支持函数嵌套,所以这种情况一般不会发生)则直接停止虚拟运行退出模拟
 *	如果是在保护函数外也退出模拟,退出被保护的函数
 */
CPU_STATUS __INTERNAL_FUNC__ ChaosVmCpuRetFlowControl(PCHAOSVM_CPU pCPU, __void *pUserContext, __word wNewSeg, __address addrNewAddress, __byte bOpcodeLength) {
	PCHAOSVM_RUNTIME pRuntime = (PCHAOSVM_RUNTIME)(pUserContext);
	PCHAOSVMP_PROCEDURE pProcedure = &(pRuntime->Procedure);
	/*
	 * 如果发现目标地址是在保护范围之外, 则EIP发生改变, 将此EIP重新压入虚拟机的堆栈, 并终止虚拟机运行
	 */ 
	if ((addrNewAddress < pProcedure->addrProcedureMemoryAddress) || 
		(addrNewAddress >= pProcedure->addrProcedureMemoryAddress + pProcedure->iSize)) {
		return __CPU_STATUS_HOOK_STOP_CPU__;
	}

	/*
	 * 在保护范围内,直接退出
	 * 在chaosvmp阶段,JMPTARGET结构无法动态获取它弹出的地址
	 */
	return __CPU_STATUS_HOOK_STOP_CPU__;
}

/*
 * 参数:
 *	pCPU:虚拟CPU结构
 *	pUserContext:自定义内容结构,当前设定为当前被保护函数的运行环境时
 *	wNewSeg:CALL后新的段寄存器
 *	addrNewAddress:CALL后的地址
 *	bOpcodeLength:OP的长度
 *
 * 介绍:
 *	在虚拟环境中运行JMP指令,如果在保护范围外,退出虚拟机, 并且将要跳转的目标地址压入虚拟堆栈内, 这样当退出时, 将直接返回到压入的新地址
 *	如果在保护范围内,向目标地址表查询地址如果查询到则为内部地址返回__CPU_STATUS_HOOK_NOT_HANDLE__,如果查询不到则为外部地址
 *	返回__CPU_STATUS_HOOK_STOP_CPU__跳出当前被保护的范围
 *	返回__CPU_STATUS_HOOK_NOT_HANDLE__跳到保护范围内
 *	返回__CPU_STATUS_HOOK_HANDLE__暂时无定义
 */
CPU_STATUS __INTERNAL_FUNC__ ChaosVmCpuJmpFlowControl(PCHAOSVM_CPU pCPU, __void *pUserContext, __word wNewSeg, __address addrNewAddress, __byte bOpcodeLength) {
	PCHAOSVM_RUNTIME pRuntime = (PCHAOSVM_RUNTIME)(pUserContext);
	PCHAOSVMP_PROCEDURE pProcedure = &(pRuntime->Procedure);
	PCHAOSVMP_JMPTARGET_INST pTargetJmpInst = NULL;
	//__dword dwKey = 0;

	/*
	 * 如果是跳转到保护函数范围之外
	 */
	if ((addrNewAddress < pProcedure->addrProcedureMemoryAddress) || 
		(addrNewAddress >= pProcedure->addrProcedureMemoryAddress + pProcedure->iSize)) {
		/*
		 * 在保护范围外
		 * 退出虚拟机, 并且将要跳转的目标地址压入虚拟堆栈内, 这样当退出时, 将直接返回到压入的新地址
		 */
__jmp_out:
		ChaosVmCpuPushDword(pCPU, addrNewAddress);
		return __CPU_STATUS_HOOK_STOP_CPU__;
	}

	/*
	 * 保护范围内
	 * 向目标地址表查询地址
	 * 如果得到处理
	 */
	pTargetJmpInst = QueryJmpTargetInstFromTable(pRuntime->iIndex, addrNewAddress);
	if (!pTargetJmpInst) goto __jmp_out;//查询不到则为外部地址
	//pRuntime->iInstIndex = pTargetJmpInst->iInstIndex;
	//// 如果是函数头, 则使用初始化Key
	//if (pTargetJmpInst->iPrevInstRandLen == 0)
	//	dwKey = __GetInitKey__(pCPU);
	//else {
	//	__address addrNowEip = pCPU->CurrentRegistersStatus.EIP;
	//	__memory pPoint = ChaosVmSwitchRel2VisBase(pRuntime, addrNowEip);
	//	dwKey = ChaosVmHash(pPoint - pTargetJmpInst->iPrevInstRandLen, pTargetJmpInst->iPrevInstRandLen);//使用上一条指令的部分密文计算密钥
	//}

	///*
	// * 更新密钥
	// */
	//pRuntime->dwKey = dwKey;
	return __CPU_STATUS_HOOK_NOT_HANDLE__;
}

/*
 * 参数:
 *	pCPU:虚拟CPU结构
 *	pUserContext:自定义内容结构,当前设定为当前被保护函数的运行环境时
 *	wNewSeg:CALL后新的段寄存器
 *	addrNewAddress:CALL后的地址
 *	bOpcodeLength:OP的长度
 *
 * 介绍:
 *	在虚拟环境中运行JCC指令,如果在保护范围外,退出虚拟机, 并且将要跳转的目标地址压入虚拟堆栈内, 这样当退出时, 将直接返回到压入的新地址
 *	如果在保护范围内,向目标地址表查询地址如果查询到则为内部地址返回__CPU_STATUS_HOOK_NOT_HANDLE__,如果查询不到则为外部地址
 *	返回__CPU_STATUS_HOOK_STOP_CPU__跳出当前被保护的范围
 *	返回__CPU_STATUS_HOOK_NOT_HANDLE__跳到保护范围内
 *	返回__CPU_STATUS_HOOK_HANDLE__暂时无定义
 */
CPU_STATUS __INTERNAL_FUNC__ ChaosVmCpuJccFlowControl(PCHAOSVM_CPU pCPU, __void *pUserContext, __address addrNewAddress, __byte bOpcodeLength) {
	PCHAOSVM_RUNTIME pRuntime = (PCHAOSVM_RUNTIME)(pUserContext);
	PCHAOSVMP_PROCEDURE pProcedure = &(pRuntime->Procedure);
	PCHAOSVMP_JMPTARGET_INST pTargetJmpInst = NULL;
	//__dword dwKey = 0;

	/*
	 * 如果是跳转到保护函数范围之外
	 */
	if ((addrNewAddress < pProcedure->addrProcedureMemoryAddress) || 
		(addrNewAddress >= pProcedure->addrProcedureMemoryAddress + pProcedure->iSize)) {
		/*
		 * 在保护范围外
		 * 退出虚拟机, 并且将要跳转的目标地址压入虚拟堆栈内, 这样当退出时, 将直接返回到压入的新地址
		 */
__jmp_out:
		ChaosVmCpuPushDword(pCPU, addrNewAddress);
		return __CPU_STATUS_HOOK_STOP_CPU__;
	}

	/*
	 * 在保护范围内
	 * 向目标地址表查询地址
	 * 如果得到目标地址,则重新核算下一个Key, 以及指令索引
	 */
	pTargetJmpInst = QueryJmpTargetInstFromTable(pRuntime->iIndex, addrNewAddress);
	if (!pTargetJmpInst) goto __jmp_out;//查询不到则为外部地址
	//pRuntime->iInstIndex = pTargetJmpInst->iInstIndex;
	//// 如果是函数头, 则使用初始化Key
	//if (pTargetJmpInst->iPrevInstRandLen == 0)
	//	dwKey = __GetInitKey__(pCPU);
	//else {
	//	__address addrNowEip = pCPU->CurrentRegistersStatus.EIP;
	//	__memory pPoint = ChaosVmSwitchRel2VisBase(pRuntime, addrNowEip);
	//	dwKey = ChaosVmHash(pPoint - pTargetJmpInst->iPrevInstRandLen, pTargetJmpInst->iPrevInstRandLen);//使用上一条指令的部分密文计算密钥
	//}

	///*
	// * 更新密钥
	// */
	//pRuntime->dwKey = dwKey;
	return __CPU_STATUS_HOOK_NOT_HANDLE__;
}

/*
 * 参数:
 *	pCPU:虚拟CPU结构
 *	pUserContext:自定义结构
 *
 * 介绍:
 * 在读取Opcode到缓存之后, 指令运行前 执行
 */
__void __INTERNAL_FUNC__ ChaosVmCpuExecuteEachInstruction(PCHAOSVM_CPU pCPU, __void *pUserContext) {
	return;
}

/*
 * 参数:
 *	pCPU:虚拟CPU结构
 *	pUserContext:自定义结构
 *
 * 介绍:
 * 在指令运行完毕后执行,每次指令运行完毕后增加指令索引计数,并且将偏移量字段清0
 */
__void __INTERNAL_FUNC__ ChaosVmCpuExecuteEachInstructionCompleted(PCHAOSVM_CPU pCPU, __void *pUserContext) {
	/*
	 * 增加指令索引
	 */
	PCHAOSVM_RUNTIME pRuntime = (PCHAOSVM_RUNTIME)pUserContext;
	(pRuntime->iInstIndex)++;
	pRuntime->iDispSize = 0;//偏移量清0
	return;
}

/*
 * 参数:
 *	pCPU:虚拟CPU结构
 *	pUserContext:自定义结构
 *	bModrmByte:要解码的MODRM位
 *
 * 介绍:
 *	解码当前的MODRM位
 */
__byte __INTERNAL_FUNC__ ChaosVmCpuBuildModrm(PCHAOSVM_CPU pCPU, __void *pUserContext, __byte bModrmByte) {
	__byte bModrm = 0;
	PCHAOSVM_RUNTIME pRuntime = __GetRunTime__(pCPU);
	__memory *pModRmTbl = (__memory)&(pRuntime->Procedure.ModRmTbl);
	__byte bMOD = 0, bRO = 0, bRM = 0;
	__dword dwLocal = 0;
	__logic_memcpy__((__memory)&dwLocal, pModRmTbl, sizeof(__byte) * 3);
	switch (dwLocal) {
		case 0x0102:{//"\x00\x01\x02"
			bModrm = bModrmByte;
		}break;
		case 0x0201:{//"\x00\x02\x01"
			bMOD = (bModrmByte & 0xC0) >> 6;
			bRO = (bModrmByte & 0x07);
			bRM = (bModrmByte & 0x38) >> 3;
			bModrm |= (bMOD << 6);
			bModrm |= (bRO << 3);
			bModrm |= (bRM);
		}break;
		case 0x010002:{//"\x01\x00\x02"
			bMOD = (bModrmByte & 0x18) >> 3;
			bRO = (bModrmByte & 0xE0) >> 5;
			bRM = (bModrmByte & 0x07);
			bModrm |= (bMOD << 6);
			bModrm |= (bRO << 3);
			bModrm |= (bRM);
		}break;
		case 0x010200:{//"\x01\x02\x00"
			bMOD = (bModrmByte & 0x03);
			bRO = (bModrmByte & 0xE0) >> 5;
			bRM = (bModrmByte & 0x1C) >> 2;
			bModrm |= (bMOD << 6);
			bModrm |= (bRO << 3);
			bModrm |= (bRM);
		}break;
		case 0x020001:{//"\x02\x00\x01"
			bMOD = (bModrmByte & 0x18) >> 3;
			bRO = (bModrmByte & 0x07);
			bRM = (bModrmByte & 0xE0) >> 5;
			bModrm |= (bMOD << 6);
			bModrm |= (bRO << 3);
			bModrm |= (bRM);
		}break;
		case 0x020100:{//"\x02\x01\x00"
			bMOD = (bModrmByte & 0x03);
			bRO = (bModrmByte & 0x1C) >> 2;
			bRM = (bModrmByte & 0xE0) >> 5;
			bModrm |= (bMOD << 6);
			bModrm |= (bRO << 3);
			bModrm |= (bRM);
		}break;
	}
	
	return bModrm;
}

/*
 * 参数:
 *	pCPU:虚拟CPU结构
 *	pUserContext:自定义结构
 *	bSibByte:要解码的SIB位
 *
 * 介绍:
 *	解码当前的SIB位
 */
__byte __INTERNAL_FUNC__ ChaosVmCpuBuildSib(PCHAOSVM_CPU pCPU, __void *pUserContext, __byte bSibByte) {
	__byte bSib = 0;
	PCHAOSVM_RUNTIME pRuntime = __GetRunTime__(pCPU);
	__byte *pSibTbl = &(pRuntime->Procedure.SibTbl);
	__dword dwLocal = 0;
	__byte bSS = 0, bIndex = 0, bBase = 0;
	__logic_memcpy__((__memory)&dwLocal, pSibTbl, sizeof(__byte) * 3);
	switch (dwLocal) {
		case 0x0102:{//"\x00\x01\x02"
			bSib = bSibByte;
		}break;
		case 0x0201:{//"\x00\x02\x01"
			bSS = (bSibByte & 0xC0) >> 6;
			bIndex = (bSibByte & 0x07);
			bBase = (bSibByte & 0x38) >> 3;
			bSib |= (bSS << 6);
			bSib |= (bIndex << 3);
			bSib |= (bBase);
		}break;
		case 0x010002:{//"\x01\x00\x02"
			bSS = (bSibByte & 0x18) >> 3;
			bIndex = (bSibByte & 0xE0) >> 5;
			bBase = (bSibByte & 0x07);
			bSib |= (bSS << 6);
			bSib |= (bIndex << 3);
			bSib |= (bBase);
		}break;
		case 0x010200:{//"\x01\x02\x00"
			bSS = (bSibByte & 0x03);
			bIndex = (bSibByte & 0xE0) >> 5;
			bBase = (bSibByte & 0x1C) >> 2;
			bSib |= (bSS << 6);
			bSib |= (bIndex << 3);
			bSib |= (bBase);
		}break;
		case 0x020001:{//"\x02\x00\x01"
			bSS = (bSibByte & 0x18) >> 3;
			bIndex = (bSibByte & 0x07);
			bBase = (bSibByte & 0xE0) >> 5;
			bSib |= (bSS << 6);
			bSib |= (bIndex << 3);
			bSib |= (bBase);
		}break;
		case 0x020100:{//"\x02\x01\x00"
			bSS = (bSibByte & 0x03);
			bIndex = (bSibByte & 0x1C) >> 2;
			bBase = (bSibByte & 0xE0) >> 5;
			bSib |= (bSS << 6);
			bSib |= (bIndex << 3);
			bSib |= (bBase);
		}break;
	}

	return bSib;
}

__void __INTERNAL_FUNC__ ChaosVmSetDefRunTables(PCHAOSVM_CPU pCPU) {
	__logic_memcpy__(pCPU->PargsDispatchTableEntryFF, g_ChaosVmCpuDefOpcodeExtensionTableForOne_FF, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	__logic_memcpy__(pCPU->PargsDispatchTableEntryFE, g_ChaosVmCpuDefOpcodeExtensionTableForOne_FE, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	__logic_memcpy__(pCPU->PargsDispatchTableEntryC6, g_ChaosVmCpuDefOpcodeExtensionTableForOne_C6, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	__logic_memcpy__(pCPU->PargsDispatchTableEntryC7, g_ChaosVmCpuDefOpcodeExtensionTableForOne_C7, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08 * 2);
	__logic_memcpy__(pCPU->PargsDispatchTableEntryF6, g_ChaosVmCpuDefOpcodeExtensionTableForOne_F6, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	__logic_memcpy__(pCPU->PargsDispatchTableEntryF7, g_ChaosVmCpuDefOpcodeExtensionTableForOne_F7, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	__logic_memcpy__(pCPU->PargsDispatchTableEntry80, g_ChaosVmCpuDefOpcodeExtensionTableForOne_80, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	__logic_memcpy__(pCPU->PargsDispatchTableEntry81, g_ChaosVmCpuDefOpcodeExtensionTableForOne_81, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	__logic_memcpy__(pCPU->PargsDispatchTableEntry83, g_ChaosVmCpuDefOpcodeExtensionTableForOne_83, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	__logic_memcpy__(pCPU->PargsDispatchTableEntry8F, g_ChaosVmCpuDefOpcodeExtensionTableForOne_8F, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	__logic_memcpy__(pCPU->PargsDispatchTableEntryC0, g_ChaosVmCpuDefOpcodeExtensionTableForOne_C0, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	__logic_memcpy__(pCPU->PargsDispatchTableEntryC1, g_ChaosVmCpuDefOpcodeExtensionTableForOne_C1, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	__logic_memcpy__(pCPU->PargsDispatchTableEntryD0, g_ChaosVmCpuDefOpcodeExtensionTableForOne_D0, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	__logic_memcpy__(pCPU->PargsDispatchTableEntryD1, g_ChaosVmCpuDefOpcodeExtensionTableForOne_D1, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	__logic_memcpy__(pCPU->PargsDispatchTableEntryD2, g_ChaosVmCpuDefOpcodeExtensionTableForOne_D2, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	__logic_memcpy__(pCPU->PargsDispatchTableEntryD3, g_ChaosVmCpuDefOpcodeExtensionTableForOne_D3, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	__logic_memcpy__(pCPU->PargsDispatchTableEntry0FBA, g_ChaosVmCpuDefOpcodeExtensionTableForTwo_0FBA, sizeof(PARGS_DISPATCH_TABLE_ENTRY) * 0x08);
	// 2012.2.8修复,原先复制OPCODE表时使用"0xFF"个数,所以少复制一个
	__logic_memcpy__(pCPU->OneOpcodeTableEntry, g_ChaosVmCpuDefOneByteOpcodeTable, sizeof(ONE_OPCODE_TABLE_ENTRY) * 0x100);
	__logic_memcpy__(pCPU->TwoByteOpcodeTableEntry, g_ChaosVmCpuDefTwoByteOpcodeTable, sizeof(TWO_BYTE_OPCODE_TABLE_ENTRY) * 0x100);
	__logic_memcpy__(pCPU->EFlagConditionTableEntry, g_ChaosVmCpuDefEFlagContionTable, sizeof(EFLAG_CONDITION_TABLE_ENTRY) * 0x10);
	__logic_memcpy__(pCPU->ModrmByteAnalyseRoutineTableEntry, g_ChaosVmCpuDefModRMAnalyseRoutineTable, sizeof(MODRM_BYTE_ANALYSE_ROUTINE_TABLE_ENTRY) * 0x02);
}

__void __INTERNAL_FUNC__ ChaosVmReSetOpcodeDispatchFunctionByTable(PCHAOSVM_CPU pCPU, __byte *pOpcodeTbl1, __byte *pOpcodeTbl2) {
	__integer i = 0;
	PONE_OPCODE_TABLE_ENTRY pOneByteOpcodeTable = NULL;
	PTWO_BYTE_OPCODE_TABLE_ENTRY pTwoByteOpcodeTable = NULL;
	ONE_OPCODE_TABLE_ENTRY OneOpcodeTableEntryTmp = {0};
	TWO_BYTE_OPCODE_TABLE_ENTRY TwoByteOpcodeTableEntryTmp = {0};
	__bool bSwitched[0x100] = {FALSE};

	ChaosVmSetDefRunTables(pCPU);

	pOneByteOpcodeTable = (PONE_OPCODE_TABLE_ENTRY)&(pCPU->OneOpcodeTableEntry);
	pTwoByteOpcodeTable = (PTWO_BYTE_OPCODE_TABLE_ENTRY)&(pCPU->TwoByteOpcodeTableEntry);

	// 第一张表
	for (i = 0; i < 0x100; i++) {
		__byte bTargetIndex = pOpcodeTbl1[i];
		if (bSwitched[i]) continue;//如果已经得到了交换则继续

		__logic_memcpy__((__memory)(&OneOpcodeTableEntryTmp), (__memory)(&pOneByteOpcodeTable[i]), sizeof(ONE_OPCODE_TABLE_ENTRY));
		__logic_memcpy__((__memory)(&pOneByteOpcodeTable[i]), (__memory)(&pOneByteOpcodeTable[bTargetIndex]), sizeof(ONE_OPCODE_TABLE_ENTRY));
		__logic_memcpy__((__memory)(&pOneByteOpcodeTable[bTargetIndex]), (__memory)(&OneOpcodeTableEntryTmp), sizeof(ONE_OPCODE_TABLE_ENTRY));

		// 设置以交换标志
		bSwitched[i] = TRUE;
		bSwitched[bTargetIndex] = TRUE;
	}

	// 第二张表
	__logic_memset__(bSwitched, FALSE, 0x100);
	for (i = 0; i < 0x100; i++) {
		__byte bTargetIndex = pOpcodeTbl2[i];
		if (bSwitched[i]) continue;//如果已经得到了交换则继续

		__logic_memcpy__((__memory)(&TwoByteOpcodeTableEntryTmp), (__memory)(&pTwoByteOpcodeTable[i]), sizeof(TWO_BYTE_OPCODE_TABLE_ENTRY));
		__logic_memcpy__((__memory)(&pTwoByteOpcodeTable[i]), (__memory)(&pTwoByteOpcodeTable[bTargetIndex]), sizeof(TWO_BYTE_OPCODE_TABLE_ENTRY));
		__logic_memcpy__((__memory)(&pTwoByteOpcodeTable[bTargetIndex]), (__memory)(&TwoByteOpcodeTableEntryTmp), sizeof(TWO_BYTE_OPCODE_TABLE_ENTRY));

		// 设置以交换标志
		bSwitched[i] = TRUE;
		bSwitched[bTargetIndex] = TRUE;
	}
}

__void __INTERNAL_FUNC__ ChaosVmReSetOpcodeDispatchFunction(__address addrImageBase, PCHAOSVM_CPU pCPU) {
	__integer i = 0, j = 0;

	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntryFF[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntryFE[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntryC6[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x02; i++) {
		for (j = 0; j < 0x08; j++) {
			(__address)(pCPU->PargsDispatchTableEntryC7[i][j].DispatchFunction) += addrImageBase;
		}
	}
	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntryF6[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntryF7[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntry80[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntry81[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntry83[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntry8F[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntryC0[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntryC1[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntryD0[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntryD1[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntryD2[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntryD3[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x08; i++) {
		(__address)(pCPU->PargsDispatchTableEntry0FBA[i].DispatchFunction) += addrImageBase;
	}

	for (i = 0; i < 0x0100; i++) {
		(__address)(pCPU->OneOpcodeTableEntry[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x100; i++) {
		(__address)(pCPU->TwoByteOpcodeTableEntry[i].DispatchFunction) += addrImageBase;
	}
	for (i = 0; i < 0x10; i++) {
		(__address)(pCPU->EFlagConditionTableEntry[i].FlagTest) += addrImageBase;
	}
	for (i = 0; i < 0x02; i++) {
		(__address)(pCPU->ModrmByteAnalyseRoutineTableEntry[i].AnalyseRoutine) += addrImageBase;
	}
}

/*
 * 参数:
 *	pCPU:虚拟CPU结构
 *
 * 介绍:
 * 为虚拟CPU添加钩子操作,当指令运行前运行这些函数
 */
__void __INTERNAL_FUNC__ ChaosVmSetHook(PCHAOSVM_CPU pCPU) {
	ChaosVmCpuSetHook(pCPU, HOOK_CALL, ChaosVmCpuCallFlowControl);
	ChaosVmCpuSetHook(pCPU, HOOK_RET, ChaosVmCpuRetFlowControl);
	ChaosVmCpuSetHook(pCPU, HOOK_JMP, ChaosVmCpuJmpFlowControl);
	ChaosVmCpuSetHook(pCPU, HOOK_JCC, ChaosVmCpuJccFlowControl);
	ChaosVmCpuSetHook(pCPU, HOOK_EXECUTEEACHINSTRUCTION, ChaosVmCpuExecuteEachInstruction);
	ChaosVmCpuSetHook(pCPU, HOOK_EXECUTEEACHINSTRUCTIONCOMPLETED, ChaosVmCpuExecuteEachInstructionCompleted);
}

/*
 * 参数:
 *	pChaosVmRuntime:运行环境时
 *
 * 介绍:
 * 首先进入解密算法部分,解密完成后进入VM运行部分,在VM运行的过程中,如果发现存在剩余数据,则进入剩余数据解码
 */
CPU_STATUS __API__ ChaosVm(PCHAOSVM_RUNTIME pChaosVmRuntime) {
	PCHAOSVMP_PROCEDURE pVmpProcedure = &(pChaosVmRuntime->Procedure);
	__integer iIndex = pChaosVmRuntime->iIndex;
	__integer i = 0;
	__dword dwJmpTargetKey = 0;
	__integer iSize = pVmpProcedure->iSize;
	__integer iInstIndex = 0;
	__integer iMaxInstCount = pVmpProcedure->iInstCount;
	PCHAOSVMP_JMPTARGET pJmpTarget = &(pVmpProcedure->JmpTargetRecord);
	PCHAOSVM_CPU pChaosVmCpu = &(pChaosVmRuntime->Cpu);
	CPU_STATUS Status;

	// 断点
	__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Entry ChaosVm", pVmpProcedure);
	__PrintDbgInfo_DebugerWriteFormatStringInteger__("<chaosvm>Procedure index = ", iIndex);

	/*
	 * 解密JMPTARGET,并将节点输入到树中
	 */
	__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Decrypt JmpTarget", pVmpProcedure);
	dwJmpTargetKey = ChaosVmHash(pVmpProcedure->pVmpProcedure, pVmpProcedure->iSize);//以当前函数的密体作为计算跳转结构密钥
	__PrintDbgInfo_DebugerWriteFormatStringInteger__("<chaosvm>JmpTarget's Key = ", dwJmpTargetKey);
	ChaosVmDecrypt((__memory)pJmpTarget, sizeof(CHAOSVMP_JMPTARGET), dwJmpTargetKey, (__memory)pJmpTarget);
	InitJmpTargetDataBaseTable(iIndex);//初始化自己的跳转目标符号库
	__PrintDbgInfo_DebugerWriteFormatStringInteger__("<chaosvm>Total of Address in JmpTarget = ", pJmpTarget->iAddressCount);
	__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Already entry decrypt JmpTarget loop", pVmpProcedure);
	for (i = 0; i < pJmpTarget->iAddressCount; i++) {
		// 合成当前跳转目标的地址
		pJmpTarget->pInstList[i].addrAddress = pVmpProcedure->addrProcedureMemoryAddress + pJmpTarget->pInstList[i].ofOffsetByProcedure;//重新设置
		__PrintDbgInfo_DebugerWriteFormatStringInteger__("<chaosvm>Add JmpTarget node to table, address = ", pJmpTarget->pInstList[i].addrAddress);
		AddJmpTargetInstToTable(iIndex, pJmpTarget->pInstList[i].addrAddress, &(pJmpTarget->pInstList[i]));
	}

	/*
	 * 更新自己的密钥
	 * 如果是头函数那么则使用头函数提供的KEY,如果不是则使用它的上一个被保护的函数的密文HASH值作为KEY
	 */
	__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Updata Key", pVmpProcedure);
	if (pVmpProcedure->bHeader)//如果是头函数
		pChaosVmRuntime->dwKey = pVmpProcedure->dwKey;
	else
		pChaosVmRuntime->dwKey = ChaosVmHash((__memory)(pChaosVmRuntime->addrPrevProcedureAddress), pChaosVmRuntime->iPrevProcedureSize);

	/*
	 * 初始化CPU
	 */
	__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Init ChaosVm CPU", pVmpProcedure);
	ChaosVmCpuInit(pChaosVmCpu);

	// 重新修订指令分派函数的基地址
#if defined(__CHAOSVM_MODE_EMULATION__)
	{
		__byte Opcode1Tbl[0x100] = {0};
		__byte Opcode2Tbl[0x100] = {0};
		XorArray(__CHAOSVM_DECRYPT_OPCODE_TABLE_KEY_EMULATION_MODE__, pChaosVmRuntime->Opcode1Tbl, Opcode1Tbl, 0x100);
		XorArray(__CHAOSVM_DECRYPT_OPCODE_TABLE_KEY_EMULATION_MODE__, pChaosVmRuntime->Opcode2Tbl, Opcode2Tbl, 0x100);
		ChaosVmReSetOpcodeDispatchFunctionByTable(pChaosVmCpu, Opcode1Tbl, Opcode2Tbl);
	}
#else
	{
		ChaosVmReSetOpcodeDispatchFunction(g_addrNowChaosVmImageBase, pChaosVmCpu);
	}
#endif
	ChaosVmCpuSetUserContext(pChaosVmCpu, (__void *)pChaosVmRuntime);
	ChaosVmCpuSetMmAccessRoutine(pChaosVmCpu, ChaosVmMmRead, ChaosVmMmWrite, ChaosVmMmCodeRead, ChaosVmMmReadDataPart, ChaosVmMmReadDispPart);
	ChaosVmSetHook(pChaosVmCpu);
	ChaosVmCpuBuildModrmSibRoutine(pChaosVmCpu, ChaosVmCpuBuildModrm, ChaosVmCpuBuildSib);
	ChaosVmCpuSetInterruptHandler(pChaosVmCpu, ChaosVmCpuInterruptHandle);

	/*
	 * 执行虚拟化运行
	 */
	__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Already goto ChaosVmCpuRunToOffset", pVmpProcedure);
	pChaosVmCpu->CurrentRegistersStatus.EIP = pVmpProcedure->addrProcedureMemoryAddress;//设置为真实EIP
	Status = ChaosVmCpuRunToOffset(pChaosVmCpu, iSize);

	return Status;//直接返回,其余操作由调用函数完成
}

/*
 * 参数:
 *	pRuntime:混乱虚拟机运行环境时
 *
 * 介绍:
 *	释放运行环境时
 */
__void __INTERNAL_FUNC__ ChaosVmReleaseRuntime(PCHAOSVM_RUNTIME pRuntime) {
	__memory pStack = pRuntime->pStack;
	//if (pStack) __logic_delete__(pStack);
	if (pStack) __logic_memset__(pStack, 0, pRuntime->iStackSize);
	__logic_delete__(pRuntime);
}

/*
 * 参数:
 *	pChaosVmRuntime:混乱虚拟机运行环境时
 *
 * 介绍:
 *	调用此函数进入虚拟环境,此函数被ChaosVmInvokeDoor调用
 */
__void __INTERNAL_FUNC__ ChaosVmInvoke(PCHAOSVM_RUNTIME pChaosVmRuntime) {
	__dword dwEFlag = 0, dwEAX = 0, dwECX = 0, dwEDX = 0, dwEBX = 0, dwESP = 0, dwEBP = 0, dwESI = 0, dwEDI = 0;
	
	// 断点
	__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Entry ChaosVmInvoke", pChaosVmRuntime);

	// 运行混乱虚拟机
	ChaosVm(pChaosVmRuntime);

	/*
	 * 将现存在的寄存器值统统压入原始堆栈中
	 * 当虚拟环境运行完毕,这里负责将虚拟运行环境的寄存器值设置给真实环境
	 * 并且完成切栈操作
	 */
	__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Exit ChaosVmInvoke", pChaosVmRuntime);
	dwEFlag = pChaosVmRuntime->Cpu.CurrentRegistersStatus.BasicRegister.EFlag.BitArray;
	dwEAX = pChaosVmRuntime->Cpu.CurrentRegistersStatus.BasicRegister.GeneralRegister.Interface.EAX.dwValue32;
	dwECX = pChaosVmRuntime->Cpu.CurrentRegistersStatus.BasicRegister.GeneralRegister.Interface.ECX.dwValue32;
	dwEDX = pChaosVmRuntime->Cpu.CurrentRegistersStatus.BasicRegister.GeneralRegister.Interface.EDX.dwValue32;
	dwEBX = pChaosVmRuntime->Cpu.CurrentRegistersStatus.BasicRegister.GeneralRegister.Interface.EBX.dwValue32;
	dwESP = pChaosVmRuntime->Cpu.CurrentRegistersStatus.BasicRegister.GeneralRegister.Interface.ESP.dwValue32;
	dwEBP = pChaosVmRuntime->Cpu.CurrentRegistersStatus.BasicRegister.GeneralRegister.Interface.EBP.dwValue32;
	dwESI = pChaosVmRuntime->Cpu.CurrentRegistersStatus.BasicRegister.GeneralRegister.Interface.ESI.dwValue32;
	dwEDI = pChaosVmRuntime->Cpu.CurrentRegistersStatus.BasicRegister.GeneralRegister.Interface.EDI.dwValue32;
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwEFlag);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwEAX);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwECX);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwEDX);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwEBX);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwESP);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwEBP);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwESI);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), dwEDI);
	ChaosVmCpuPushDword(&(pChaosVmRuntime->Cpu), (__dword)pChaosVmRuntime);//为了能够顺利切栈并释放运行环境,这里把当前的运行时指针压入当前堆栈
	dwESP = pChaosVmRuntime->Cpu.CurrentRegistersStatus.BasicRegister.GeneralRegister.Interface.ESP.dwValue32;//取得现在的ESP的值
	// 恢复物理环境后自动切栈到原始状态
	__asm {
		mov esp, dwESP												; 切栈操作
		call ChaosVmReleaseRuntime									; esp的执行已然是要释放的运行环境时
		add esp, 0x04												; ChaosVmReleaseRuntime是C调用,这里清楚它的堆栈
		popad														; 这里恢复物理寄存器
		popfd														; 恢复存在于当前栈顶的标志寄存器
		ret															; 直接范围到最原始的返回地址中
	}

	// 一但切栈后以下代码将不能访问局部变量
	return;
}

/*
 * 参数:
 *	iIndex:当前被保护函数的索引
 *	pReverseBasicRegisters:基础寄存器保存结构,但是记录的方向与BASIC_REGISTERS结构相反
 *
 * 介绍:
 *	调用混乱虚拟机的门函数,被保护函数由此进入混乱虚拟机,此函数的地址由附加器动态设置给ChaosVmInvokeStub,并由Stub调用Door,这里做的主要操作是
 *	为当前保护函数的环境运行时分配空间以及把当前的物理环境状态设置给虚拟机的虚拟环境,最后一个重要的操作是切栈操作.要给真实的CPU中的堆栈指针esp
 *	分配一个新的空间,而原来的栈空间提供给虚拟环境使用,最终使用ChaosVmInvoke进入虚拟机保护
 */
__void __API__ ChaosVmInvokeDoor(__integer iIndex, PREVERSE_BASIC_REGISTERS pReverseBasicRegisters) {
	PCHAOSVM_RUNTIME pRuntime = NULL;
	PCHAOSVM_RUNTIME pCurrRuntime = NULL;
	PCHAOSVMP_PROCEDURE pChaosVmpProcedure = NULL;
	__memory pStartStack = NULL;
	__memory pDecompressCode = NULL;

	// 初始化数据
	pRuntime = &(g_pChaosVmRuntimeList[iIndex]);//通过索引获取到当前的运行环境时
	pCurrRuntime = (PCHAOSVM_RUNTIME)__logic_new__(CHAOSVM_RUNTIME, 1);
	pChaosVmpProcedure = &(pCurrRuntime->Procedure);
	pStartStack = pRuntime->pStack + __DEF_CHAOSVM_START_STACK_POINTER__;

	// 断点
	__PrintDbgInfo_DebugerBreakPoint__("<chaosvm>Entry ChaosVmInvokeDoor", pRuntime);

	/*
	 * 设置Runtime的必须值
	 */
	//__logic_memset__(pCurrRuntime->pStack, 0, pCurrRuntime->iStackSize);
	pRuntime->Cpu.pUserContext = (__void *)pRuntime;//把运行环境时的指针设置给CPU
	__logic_memcpy__(pCurrRuntime, pRuntime, sizeof(CHAOSVM_RUNTIME));
	pCurrRuntime->iInstIndex = 0;
	pCurrRuntime->Cpu.pUserContext = (__void *)pCurrRuntime;
	__ReverseBasicRegisters2BasicRegisters__(pReverseBasicRegisters, &(pCurrRuntime->Cpu.CurrentRegistersStatus.BasicRegister));//将物理寄存器状态设置到虚拟CPU中

	/*
	 * 切换堆栈
	 * 切换堆栈之后,esp,ebp将指向新的堆栈,在切栈之前将调用ChaosVmInovke的环境设置到新的堆栈中
	 * 并设置esp的新值到要调用的地址处
	 */
	*(__address *)(pStartStack) = (__address)ChaosVmInvoke;//将要调用的地址放入
	*(__address *)(pStartStack + sizeof(__address)) = (__address)0x19831210;//任意塞入一个值作为虚假的返回值
	*(__address *)(pStartStack + sizeof(__address) * 2) = (__address)pCurrRuntime;
	__asm {
		mov esp, pStartStack
		mov ebp, esp
		ret								;使用ret直接调用ChaosVmInvoke
	}
	//ChaosVmInvoke(pRuntime);
}

// 保护DIS步骤
#include "ChaosVmDISSteps.c"
