/**
 * Appcelerator Titanium - licensed under the Apache Public License 2
 * see LICENSE in the root folder for details on the license.
 * Copyright (c) 2009 Appcelerator, Inc. All Rights Reserved.
 */

#include "input_pipe.h"
#include "buffered_input_pipe.h"

#if defined(OS_OSX)
# include "osx/osx_input_pipe.h"
#elif defined(OS_WIN32)
# include "win32/win32_input_pipe.h"
#elif defined(OS_LINUX)
# include "linux/linux_input_pipe.h"
#endif

namespace ti
{
	/*static*/
	SharedInputPipe InputPipe::CreateInputPipe()
	{
		
#if defined(OS_OSX)
		SharedInputPipe pipe = new OSXInputPipe();
#elif defined(OS_WIN32)
		SharedInputPipe pipe = new Win32InputPipe();
#elif defined(OS_LINUX)
		SharedInputPipe pipe = new LinuxInputPipe();
#endif
		pipe->sharedThis = pipe;
		return pipe;
	}
	
	InputPipe::InputPipe() : Pipe("InputPipe"), joined(false), attachedOutput(NULL), splitPipes(NULL), onRead(NULL), onClose(NULL)
	{
		logger = Logger::Get("InputPipe");
		
		//TODO doc me
		SetMethod("read", &InputPipe::_Read);
		SetMethod("readLine", &InputPipe::_ReadLine);
		SetMethod("join", &InputPipe::_Join);
		SetMethod("unjoin", &InputPipe::_Unjoin);
		SetMethod("isJoined", &InputPipe::_IsJoined);
		SetMethod("split", &InputPipe::_Split);
		SetMethod("unsplit", &InputPipe::_Unsplit);
		SetMethod("isSplit", &InputPipe::_IsSplit);
		SetMethod("attach", &InputPipe::_Attach);
		SetMethod("detach", &InputPipe::_Detach);
		SetMethod("isAttached", &InputPipe::_IsAttached);
		SetMethod("isPiped", &InputPipe::_IsPiped);
		SetMethod("setOnRead", &InputPipe::_SetOnRead);
		SetMethod("setOnClose", &InputPipe::_SetOnClose);
	}
	
	InputPipe::~InputPipe()
	{
	}
	
	void InputPipe::DataReady(SharedInputPipe pipe)
	{
		if (pipe.isNull()) pipe = sharedThis.cast<InputPipe>();
		
		if (attachedOutput)
		{
			SharedPtr<Blob> data = pipe->Read();
			SharedValue result = (*attachedOutput)->CallNS("write", Value::NewObject(data));
		}
		else
		{
			if (onRead != NULL && !onRead->isNull())
			{
				ValueList args;
				SharedKObject event = new StaticBoundObject();
				event->SetObject("pipe", pipe);
				
				args.push_back(Value::NewObject(event));
				try
				{
					Host::GetInstance()->InvokeMethodOnMainThread(*this->onRead, args);
				}
				catch (ValueException& e)
				{
					logger->Error(e.DisplayString()->c_str());
				}
			}
		}
	}
	
	void InputPipe::Closed()
	{
		if (onClose && !onClose->isNull())
		{
			ValueList args;
			SharedKObject event = new StaticBoundObject();
			event->SetObject("pipe", sharedThis);
			
			args.push_back(Value::NewObject(event));
			
			try
			{
				Host::GetInstance()->InvokeMethodOnMainThread(*this->onClose, args);
			}
			catch (ValueException& e)
			{
				logger->Error(e.DisplayString()->c_str());
			}
		}
	}
	
	void InputPipe::Join(SharedInputPipe other)
	{
		joinedPipes.push_back(other);
		other->joined = true;
		other->joinedTo = sharedThis.cast<InputPipe>();
		
		if (joinedRead.isNull())
		{
	 		MethodCallback* joinedCallback =
				NewCallback<InputPipe, const ValueList&, SharedValue>(this, &InputPipe::JoinedRead);
			
			joinedRead = new StaticBoundMethod(joinedCallback);
		}
		
		other->onRead = new SharedKMethod(joinedRead);
	}
	
	void InputPipe::JoinedRead(const ValueList& args, SharedValue result)
	{
		DataReady(args.at(0)->ToObject().cast<InputPipe>());
	}
	
	void InputPipe::Unjoin(SharedInputPipe other)
	{
		if (other->joinedTo.get() == sharedThis.get())
		{
			other->joined = false;
			other->onRead = NULL;
			other->joinedTo = NULL;
		}
	}
	
	bool InputPipe::IsJoined()
	{
		return this->joined;
	}
	
	std::pair<SharedBufferedInputPipe,SharedBufferedInputPipe>& InputPipe::Split()
	{
		SharedPtr<BufferedInputPipe> pipe1 = new BufferedInputPipe();
		SharedPtr<BufferedInputPipe> pipe2 = new BufferedInputPipe();
		
		this->splitPipes =
			new std::pair<SharedPtr<BufferedInputPipe>, SharedPtr<BufferedInputPipe> >(pipe1, pipe2);
		
		MethodCallback* splitCallback = NewCallback<InputPipe, const ValueList&, SharedValue>(this, &InputPipe::Splitter);
		MethodCallback* closeCallback = NewCallback<InputPipe, const ValueList&, SharedValue>(this, &InputPipe::SplitterClose);
		this->onRead = new SharedKMethod(new StaticBoundMethod(splitCallback));
		this->onClose = new SharedKMethod(new StaticBoundMethod(closeCallback));
		
		return *this->splitPipes;
	}
	
	void InputPipe::Splitter(const ValueList& args, SharedValue result)
	{
		SharedPtr<BufferedInputPipe> pipe1 = this->splitPipes->first;
		SharedPtr<BufferedInputPipe> pipe2 = this->splitPipes->second;
		
		SharedPtr<Blob> blob = this->Read();
		pipe1->Append(blob);
		pipe2->Append(blob);
	}
	
	void InputPipe::SplitterClose(const ValueList& args, SharedValue result)
	{
		this->splitPipes->first->Close();
		this->splitPipes->second->Close();
	}
	
	void InputPipe::Unsplit()
	{
		if (IsSplit())
		{
			delete this->onRead;
			delete this->onClose;
			
			delete splitPipes;
		}
	}
	
	bool InputPipe::IsSplit()
	{
		return splitPipes != NULL;
	}
	
	void InputPipe::Attach(SharedKObject other)
	{
		attachedOutput = new SharedKObject(other);
	}
	
	void InputPipe::Detach()
	{
		if (attachedOutput && !attachedOutput->isNull())
		{
			delete attachedOutput;
		}
	}
	
	bool InputPipe::IsAttached()
	{
		return attachedOutput != NULL;
	}
	
	void InputPipe::SetOnRead(SharedKMethod onReadCallback)
	{
		if (this->onRead && onReadCallback.get() == this->onRead->get()) return;
		
		if (IsJoined())
		{
			joinedTo->Unjoin(sharedThis.cast<InputPipe>());
		}
		else if (IsSplit())
		{
			Unsplit();
		}
		else if (IsAttached())
		{
			Detach();
		}
		
	 	if (onReadCallback.isNull())
		{
			delete this->onRead;
		}
		else
		{
			this->onRead = new SharedKMethod(onReadCallback);
		}
	}
	
	int InputPipe::FindFirstLineFeed(char *data, int length, int *charsToErase)
	{
		int newline = -1;
		for (int i = 0; i < length; i++)
		{
			if (data[i] == '\n')
			{
				newline = i;
				*charsToErase = 1;
				break;
			}
			else if (data[i] == '\r')
			{
				if (i < length-1 && data[i+1] == '\n')
				{
					newline = i+1;
					*charsToErase = 2;
					break;
				}
			}
		}
		
		return newline;
	}
	
	void InputPipe::_Read(const ValueList& args, SharedValue result)
	{
		int bufsize = -1;
		if (args.size() > 0)
		{
			bufsize = args.at(0)->ToInt();
		}
		
		SharedPtr<Blob> blob = Read(bufsize);
		result->SetObject(blob);
	}
	
	void InputPipe::_ReadLine(const ValueList& args, SharedValue result)
	{
		SharedPtr<Blob> blob = ReadLine();
		if (blob.isNull())
		{
			result->SetNull();
		}
		else
		{
			result->SetObject(blob);
		}
	}
	
	void InputPipe::_Join(const ValueList& args, SharedValue result)
	{
		if (args.size() > 0 && args.at(0)->IsObject())
		{
			SharedInputPipe pipe = args.at(0)->ToObject().cast<InputPipe>();
			if (!pipe.isNull())
			{
				this->Join(pipe);
			}
		}
	}
	
	void InputPipe::_Unjoin(const ValueList& args, SharedValue result)
	{
		if (args.size() > 0 && args.at(0)->IsObject())
		{
			SharedInputPipe pipe = args.at(0)->ToObject().cast<InputPipe>();
			if (!pipe.isNull())
			{
				this->Unjoin(pipe);
			}
		}
	}
	
	void InputPipe::_IsJoined(const ValueList& args, SharedValue result)
	{
		result->SetBool(this->IsJoined());
	}
	
	void InputPipe::_Split(const ValueList& args, SharedValue result)
	{
		this->Split();
		SharedKList list = new StaticBoundList();
		list->Append(Value::NewObject(this->splitPipes->first));
		list->Append(Value::NewObject(this->splitPipes->second));
		result->SetList(list);
	}
	
	void InputPipe::_Unsplit(const ValueList& args, SharedValue result)
	{
		this->Unsplit();
	}
	
	void InputPipe::_IsSplit(const ValueList& args, SharedValue result)
	{
		result->SetBool(this->IsSplit());
	}
	
	void InputPipe::_Attach(const ValueList& args, SharedValue result)
	{
		if (args.size() >= 0 && args.at(0)->IsObject())
		{
			this->Attach(args.at(0)->ToObject());
		}
	}
	
	void InputPipe::_Detach(const ValueList& args, SharedValue result)
	{
		this->Detach();
	}
	
	void InputPipe::_IsAttached(const ValueList& args, SharedValue result)
	{
		result->SetBool(this->IsAttached());
	}
	
	void InputPipe::_IsPiped(const ValueList& args, SharedValue result)
	{
		result->SetBool(this->IsPiped());
	}
	
	void InputPipe::_SetOnRead(const ValueList& args, SharedValue result)
	{
		if (args.size() >= 0 && args.at(0)->IsMethod())
		{
			SetOnRead(args.at(0)->ToMethod());
		}
	}
	
	void InputPipe::_SetOnClose(const ValueList& args, SharedValue result)
	{
		if (args.size() >= 0 && args.at(0)->IsMethod())
		{
			this->onClose = new SharedKMethod(args.at(0)->ToMethod());
		}
	}
}