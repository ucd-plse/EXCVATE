module squared_hinge_losses
    implicit none

    contains

        real function squared_hinge_loss1(y, t)
            real :: y, t
            squared_hinge_loss1 = (max(0.0, 1-y*t))**2
        end function

        real function squared_hinge_loss2(y, t)
            real :: y, t
            squared_hinge_loss2 = (max(1-y*t, 0.0))**2
        end function

end module 