/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

public class A extends B implements I {
  public int field;
  public void voidMethod() {
    AbstractInvokeExactTest.STATUS = "A.voidMethod";
  }

  @Override
  public void overrideMe() {
    AbstractInvokeExactTest.STATUS = "A.overrideMe";
  }

  public void throwException() {
    AbstractInvokeExactTest.STATUS = "A.throwException";
    throw new MyRuntimeException();
  }

  public double returnDouble() {
    return 42.0d;
  }

  public int returnInt() {
    return 42;
  }

  private int privateReturnInt() {
    return 1042;
  }

  public static String staticMethod(A a) {
    return "staticMethod";
  }

  public static double staticMethod() {
    return 41.0d;
  }
}
