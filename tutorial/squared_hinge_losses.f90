module squared_hinge_losses
    implicit none

    contains

        real function squared_hinge_loss1(x)
            real :: x
            squared_hinge_loss1 = (max(0.0, 1-x))**2
        end function

        real function squared_hinge_loss2(x)
            real :: x
            squared_hinge_loss2 = (max(1-x, 0.0))**2
        end function

end module 